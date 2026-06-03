/*
 * server.c - Çok İstemcili TCP Dosya Sunucusu
 *
 * Mimari: Thread-per-client (pthread)
 * Senkronizasyon:
 *   - g_log_mutex  (pthread_mutex_t)  : log dosyasına seri erişim
 *   - g_files_lock (pthread_rwlock_t) : GET/LIST = okuma kilidi,
 *                                       PUT/DELETE = yazma kilidi
 *   - g_cnt_mutex  (pthread_mutex_t)  : aktif istemci sayacı
 *
 * Protokol (uzunluk-önekli, EOF hack'siz):
 *   LIST              → OK <n>\n  <ad> <boyut>\n … END\n
 *   GET <ad>          → OK <boyut>\n <boyut> ham bayt  |  ERR <mesaj>\n
 *   PUT <ad> <boyut>  → READY\n  (istemci <boyut> bayt gönderir)  → OK\n | ERR\n
 *   DELETE <ad>       → OK\n  |  ERR <mesaj>\n
 *   QUIT              → BYE\n
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <time.h>
#include <strings.h>

#include <pthread.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <dirent.h>

/* ── Sabitler ─────────────────────────────────────────────── */
#define PORT           8080
#define BACKLOG        10
#define BUF_SIZE       (64 * 1024)          /* 64 KB aktarım tamponu   */
#define MAX_FILENAME   256
#define MAX_CMD        512
#define MAX_CLIENTS    50
#define MAX_FILE_SIZE  (512LL * 1024 * 1024) /* 512 MB dosya sınırı    */
#define STORAGE_DIR    "./storage"
#define LOG_FILE       "server.log"
#define RECV_TIMEOUT   120                  /* saniye cinsinden timeout */

/* ── Global değişkenler ───────────────────────────────────── */
static volatile sig_atomic_t g_running = 1;
static int                   g_server_fd = -1;

static pthread_mutex_t  g_log_mutex   = PTHREAD_MUTEX_INITIALIZER;
static pthread_rwlock_t g_files_lock  = PTHREAD_RWLOCK_INITIALIZER;
static pthread_mutex_t  g_cnt_mutex   = PTHREAD_MUTEX_INITIALIZER;
static int              g_active_cnt  = 0;

/* ── İstemci bilgisi (thread argümanı) ───────────────────── */
typedef struct {
    int  fd;
    char ip[INET_ADDRSTRLEN];
    int  port;
} client_info_t;

/* ══════════════════════════════════════════════════════════
 *  LOGLAMA
 * ══════════════════════════════════════════════════════════ */
static void log_msg(const char *level, const char *fmt, ...)
{
    char timebuf[32];
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", tm_info);

    char msgbuf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msgbuf, sizeof(msgbuf), fmt, ap);
    va_end(ap);

    pthread_mutex_lock(&g_log_mutex);

    printf("[%s] [%s] %s\n", timebuf, level, msgbuf);
    fflush(stdout);

    FILE *lf = fopen(LOG_FILE, "a");
    if (lf) {
        fprintf(lf, "[%s] [%s] %s\n", timebuf, level, msgbuf);
        fclose(lf);
    }

    pthread_mutex_unlock(&g_log_mutex);
}

#define LOG_INFO(...)  log_msg("INFO ", __VA_ARGS__)
#define LOG_WARN(...)  log_msg("WARN ", __VA_ARGS__)
#define LOG_ERROR(...) log_msg("ERROR", __VA_ARGS__)

/* ══════════════════════════════════════════════════════════
 *  DÜŞÜK SEVİYE SOKET YARDIMCILARI
 * ══════════════════════════════════════════════════════════ */

/* Soketten '\n' gelene kadar bir satır oku. '\r' atlanır.
 * Dönüş: okunan karakter sayısı; <=0 → hata/kapatma. */
static int recv_line(int fd, char *buf, int maxlen)
{
    int i = 0;
    char c;
    while (i < maxlen - 1) {
        ssize_t n = recv(fd, &c, 1, 0);
        if (n <= 0) return (int)n;
        if (c == '\n') break;
        if (c != '\r') buf[i++] = c;
    }
    buf[i] = '\0';
    return i;
}

/* len baytı kesinlikle gönder. Dönüş: gönderilen bayt; -1 hata. */
static ssize_t send_all(int fd, const void *buf, size_t len)
{
    const char *p = (const char *)buf;
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, p + sent, len - sent, MSG_NOSIGNAL);
        if (n <= 0) return -1;
        sent += (size_t)n;
    }
    return (ssize_t)sent;
}

/* Biçimli satır gönder ('\n' otomatik eklenir). */
static int sendf(int fd, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int len = vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    if (len < 0) return -1;
    buf[len++] = '\n';
    return (send_all(fd, buf, (size_t)len) == len) ? 0 : -1;
}

/* ══════════════════════════════════════════════════════════
 *  GEÇECİ SÜRE ÖLÇÜMÜ
 * ══════════════════════════════════════════════════════════ */
static double elapsed_sec(const struct timeval *start)
{
    struct timeval now;
    gettimeofday(&now, NULL);
    return (now.tv_sec  - start->tv_sec) +
           (now.tv_usec - start->tv_usec) / 1e6;
}

/* ══════════════════════════════════════════════════════════
 *  GÜVENLİK: Dosya adı doğrulama
 * ══════════════════════════════════════════════════════════ */
static int is_valid_filename(const char *name)
{
    if (!name || name[0] == '\0' || strlen(name) >= MAX_FILENAME)
        return 0;
    /* '..' bileşeni yasak → üst dizine çıkma engeli */
    if (strstr(name, "..") != NULL)
        return 0;
    /* Dizin ayracı ve kontrol karakteri yasak */
    for (const char *p = name; *p; p++) {
        if (*p == '/' || *p == '\\' || (unsigned char)*p < 32)
            return 0;
    }
    return 1;
}

static void build_path(char *out, size_t outlen, const char *filename)
{
    snprintf(out, outlen, "%s/%s", STORAGE_DIR, filename);
}

/* ══════════════════════════════════════════════════════════
 *  KOMUT İŞLEYİCİLER
 * ══════════════════════════════════════════════════════════ */

/* LIST: depolama dizinindeki düzenli dosyaları listele */
static void handle_list(int fd, const char *ip)
{
    /* Okuma kilidi: eşzamanlı GET ile çakışmaz */
    pthread_rwlock_rdlock(&g_files_lock);

    DIR *dir = opendir(STORAGE_DIR);
    if (!dir) {
        pthread_rwlock_unlock(&g_files_lock);
        sendf(fd, "ERR Depolama klasoru acilamadi");
        LOG_ERROR("[%s] LIST: opendir hata: %s", ip, strerror(errno));
        return;
    }

    /* İki geçiş: önce say, sonra gönder (taşma riski sıfır) */
    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        char fp[512];
        snprintf(fp, sizeof(fp), "%s/%s", STORAGE_DIR, ent->d_name);
        struct stat st;
        if (stat(fp, &st) == 0 && S_ISREG(st.st_mode))
            count++;
    }

    sendf(fd, "OK %d", count);

    rewinddir(dir);
    int sent = 0;
    while ((ent = readdir(dir)) != NULL) {
        char fp[512];
        snprintf(fp, sizeof(fp), "%s/%s", STORAGE_DIR, ent->d_name);
        struct stat st;
        if (stat(fp, &st) != 0 || !S_ISREG(st.st_mode))
            continue;
        sendf(fd, "%s %lld", ent->d_name, (long long)st.st_size);
        sent++;
    }
    closedir(dir);
    pthread_rwlock_unlock(&g_files_lock);

    sendf(fd, "END");
    LOG_INFO("[%s] LIST: %d dosya listelendi", ip, sent);
}

/* GET: istemciye dosya gönder (okuma kilidi) */
static void handle_get(int fd, const char *filename, const char *ip)
{
    if (!is_valid_filename(filename)) {
        sendf(fd, "ERR Gecersiz dosya adi");
        LOG_WARN("[%s] GET engellendi: '%s'", ip, filename);
        return;
    }

    char path[512];
    build_path(path, sizeof(path), filename);

    pthread_rwlock_rdlock(&g_files_lock);

    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        pthread_rwlock_unlock(&g_files_lock);
        sendf(fd, "ERR Dosya bulunamadi");
        LOG_WARN("[%s] GET '%s': bulunamadi", ip, filename);
        return;
    }

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        pthread_rwlock_unlock(&g_files_lock);
        sendf(fd, "ERR Dosya acilamadi");
        LOG_ERROR("[%s] GET '%s': fopen: %s", ip, filename, strerror(errno));
        return;
    }

    long long filesize = (long long)st.st_size;
    sendf(fd, "OK %lld", filesize);

    char *buf = malloc(BUF_SIZE);
    if (!buf) {
        fclose(fp);
        pthread_rwlock_unlock(&g_files_lock);
        LOG_ERROR("[%s] GET '%s': malloc basarisiz", ip, filename);
        return;
    }

    struct timeval start;
    gettimeofday(&start, NULL);
    long long sent_total = 0;
    int ok = 1;
    size_t nread;

    while ((nread = fread(buf, 1, BUF_SIZE, fp)) > 0) {
        if (send_all(fd, buf, nread) < 0) { ok = 0; break; }
        sent_total += (long long)nread;
    }

    free(buf);
    fclose(fp);
    pthread_rwlock_unlock(&g_files_lock);

    double sec   = elapsed_sec(&start);
    double mbps  = (sec > 0) ? (sent_total / sec / 1048576.0) : 0.0;

    if (ok)
        LOG_INFO("[%s] GET '%s': %lld B, %.2f s, %.2f MB/s",
                 ip, filename, sent_total, sec, mbps);
    else
        LOG_ERROR("[%s] GET '%s': gonderim kesintisi (%lld/%lld B)",
                  ip, filename, sent_total, filesize);
}

/* PUT: istemciden dosya al (yazma kilidi — eşzamanlı bozulma engeli) */
static void handle_put(int fd, const char *filename,
                       long long filesize, const char *ip)
{
    if (!is_valid_filename(filename)) {
        sendf(fd, "ERR Gecersiz dosya adi");
        LOG_WARN("[%s] PUT engellendi: '%s'", ip, filename);
        return;
    }

    if (filesize < 0 || filesize > MAX_FILE_SIZE) {
        sendf(fd, "ERR Dosya boyutu siniri asildi (maks %lld MB)",
              MAX_FILE_SIZE / 1048576);
        LOG_WARN("[%s] PUT '%s': boyut siniri: %lld", ip, filename, filesize);
        return;
    }

    char path[512];
    build_path(path, sizeof(path), filename);

    pthread_rwlock_wrlock(&g_files_lock);

    FILE *fp = fopen(path, "wb");
    if (!fp) {
        pthread_rwlock_unlock(&g_files_lock);
        sendf(fd, "ERR Dosya olusturulamadi");
        LOG_ERROR("[%s] PUT '%s': fopen: %s", ip, filename, strerror(errno));
        return;
    }

    sendf(fd, "READY");

    char *buf = malloc(BUF_SIZE);
    if (!buf) {
        fclose(fp);
        unlink(path);
        pthread_rwlock_unlock(&g_files_lock);
        LOG_ERROR("[%s] PUT '%s': malloc basarisiz", ip, filename);
        return;
    }

    struct timeval start;
    gettimeofday(&start, NULL);
    long long remaining  = filesize;
    long long recv_total = 0;
    int ok = 1;

    while (remaining > 0) {
        size_t to_recv = (size_t)(remaining < BUF_SIZE ? remaining : BUF_SIZE);
        ssize_t n = recv(fd, buf, to_recv, 0);
        if (n <= 0) { ok = 0; break; }
        if (fwrite(buf, 1, (size_t)n, fp) != (size_t)n) { ok = 0; break; }
        remaining  -= n;
        recv_total += n;
    }

    free(buf);
    fclose(fp);

    if (!ok) {
        unlink(path);   /* Eksik dosyayı sil */
        pthread_rwlock_unlock(&g_files_lock);
        sendf(fd, "ERR Aktarim kesintisi, dosya silindi");
        LOG_ERROR("[%s] PUT '%s': kesinti (%lld/%lld B)",
                  ip, filename, recv_total, filesize);
        return;
    }

    pthread_rwlock_unlock(&g_files_lock);

    double sec  = elapsed_sec(&start);
    double mbps = (sec > 0) ? (recv_total / sec / 1048576.0) : 0.0;

    sendf(fd, "OK");
    LOG_INFO("[%s] PUT '%s': %lld B, %.2f s, %.2f MB/s",
             ip, filename, recv_total, sec, mbps);
}

/* DELETE: dosyayı sil (yazma kilidi) */
static void handle_delete(int fd, const char *filename, const char *ip)
{
    if (!is_valid_filename(filename)) {
        sendf(fd, "ERR Gecersiz dosya adi");
        LOG_WARN("[%s] DELETE engellendi: '%s'", ip, filename);
        return;
    }

    char path[512];
    build_path(path, sizeof(path), filename);

    pthread_rwlock_wrlock(&g_files_lock);
    int ret = unlink(path);
    int saved_errno = errno;
    pthread_rwlock_unlock(&g_files_lock);

    if (ret != 0) {
        if (saved_errno == ENOENT)
            sendf(fd, "ERR Dosya bulunamadi");
        else
            sendf(fd, "ERR Silinemedi: %s", strerror(saved_errno));
        LOG_WARN("[%s] DELETE '%s': %s", ip, filename, strerror(saved_errno));
    } else {
        sendf(fd, "OK");
        LOG_INFO("[%s] DELETE '%s': silindi", ip, filename);
    }
}

/* ══════════════════════════════════════════════════════════
 *  İSTEMCİ THREAD'İ
 * ══════════════════════════════════════════════════════════ */
static void *connection_handler(void *arg)
{
    client_info_t *info = (client_info_t *)arg;
    int   fd   = info->fd;
    char  ip[INET_ADDRSTRLEN];
    int   port = info->port;
    memcpy(ip, info->ip, sizeof(ip));
    free(info);

    /* Alım zaman aşımı: pasif istemcileri sonlandırır */
    struct timeval tv = { .tv_sec = RECV_TIMEOUT, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    pthread_mutex_lock(&g_cnt_mutex);
    g_active_cnt++;
    int active = g_active_cnt;
    pthread_mutex_unlock(&g_cnt_mutex);

    LOG_INFO("[%s:%d] Baglandi (aktif: %d)", ip, port, active);

    sendf(fd, "TCP Dosya Sunucusu v2.0 | Komutlar: LIST GET PUT DELETE QUIT");

    char cmd_buf[MAX_CMD];
    while (g_running) {
        int n = recv_line(fd, cmd_buf, sizeof(cmd_buf));
        if (n <= 0) break;  /* bağlantı kapandı veya timeout */

        /* Komut ayrıştırma */
        char cmd[64]              = {0};
        char arg1[MAX_FILENAME]   = {0};
        long long arg2            = 0;
        sscanf(cmd_buf, "%63s", cmd);

        if (strcasecmp(cmd, "LIST") == 0) {
            handle_list(fd, ip);

        } else if (strcasecmp(cmd, "GET") == 0) {
            if (sscanf(cmd_buf, "%*s %255s", arg1) < 1)
                sendf(fd, "ERR Kullanim: GET <dosya_adi>");
            else
                handle_get(fd, arg1, ip);

        } else if (strcasecmp(cmd, "PUT") == 0) {
            if (sscanf(cmd_buf, "%*s %255s %lld", arg1, &arg2) < 2)
                sendf(fd, "ERR Kullanim: PUT <dosya_adi> <boyut_bayt>");
            else
                handle_put(fd, arg1, arg2, ip);

        } else if (strcasecmp(cmd, "DELETE") == 0) {
            if (sscanf(cmd_buf, "%*s %255s", arg1) < 1)
                sendf(fd, "ERR Kullanim: DELETE <dosya_adi>");
            else
                handle_delete(fd, arg1, ip);

        } else if (strcasecmp(cmd, "QUIT") == 0) {
            sendf(fd, "BYE");
            break;

        } else {
            sendf(fd, "ERR Bilinmeyen komut. (LIST GET PUT DELETE QUIT)");
            LOG_WARN("[%s] Bilinmeyen komut: '%s'", ip, cmd);
        }
    }

    close(fd);

    pthread_mutex_lock(&g_cnt_mutex);
    g_active_cnt--;
    int remaining = g_active_cnt;
    pthread_mutex_unlock(&g_cnt_mutex);

    LOG_INFO("[%s:%d] Baglanti kesildi (aktif: %d)", ip, port, remaining);
    return NULL;
}

/* ══════════════════════════════════════════════════════════
 *  SİNYAL İŞLEYİCİ
 * ══════════════════════════════════════════════════════════ */
static void signal_handler(int sig)
{
    (void)sig;
    g_running = 0;
    if (g_server_fd >= 0) {
        close(g_server_fd);
        g_server_fd = -1;
    }
}

/* ══════════════════════════════════════════════════════════
 *  ANA FONKSİYON
 * ══════════════════════════════════════════════════════════ */
int main(void)
{
    /* Sinyal işleyicileri: düzgün kapatma */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* Kırık boru (SIGPIPE) sinyalini yoksay — send() hatayı döner */
    signal(SIGPIPE, SIG_IGN);

    /* Depolama dizini oluştur */
    if (mkdir(STORAGE_DIR, 0700) != 0 && errno != EEXIST) {
        perror("mkdir storage");
        return 1;
    }

    /* TCP soketi oluştur */
    g_server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_server_fd < 0) { perror("socket"); return 1; }

    int opt = 1;
    setsockopt(g_server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(g_server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(g_server_fd);
        return 1;
    }

    if (listen(g_server_fd, BACKLOG) < 0) {
        perror("listen");
        close(g_server_fd);
        return 1;
    }

    LOG_INFO("Sunucu baslatildi — port: %d, maks istemci: %d, timeout: %ds",
             PORT, MAX_CLIENTS, RECV_TIMEOUT);

    while (g_running) {
        struct sockaddr_in caddr;
        socklen_t clen = sizeof(caddr);
        int cfd = accept(g_server_fd, (struct sockaddr *)&caddr, &clen);

        if (cfd < 0) {
            if (g_running && errno != EINTR)
                LOG_WARN("accept: %s", strerror(errno));
            continue;
        }

        /* İstemci sınırı kontrolü */
        pthread_mutex_lock(&g_cnt_mutex);
        int count = g_active_cnt;
        pthread_mutex_unlock(&g_cnt_mutex);

        if (count >= MAX_CLIENTS) {
            sendf(cfd, "ERR Sunucu dolu (%d/%d), lutfen sonra baglanin",
                  count, MAX_CLIENTS);
            close(cfd);
            LOG_WARN("Maks istemci doldu (%d)", MAX_CLIENTS);
            continue;
        }

        client_info_t *info = malloc(sizeof(client_info_t));
        if (!info) { close(cfd); continue; }

        info->fd   = cfd;
        info->port = ntohs(caddr.sin_port);
        inet_ntop(AF_INET, &caddr.sin_addr, info->ip, sizeof(info->ip));

        pthread_t tid;
        if (pthread_create(&tid, NULL, connection_handler, info) != 0) {
            LOG_ERROR("pthread_create: %s", strerror(errno));
            free(info);
            close(cfd);
            continue;
        }
        pthread_detach(tid);
    }

    LOG_INFO("Sunucu kapatiliyor (aktif istemci: %d)...", g_active_cnt);
    pthread_mutex_destroy(&g_log_mutex);
    pthread_mutex_destroy(&g_cnt_mutex);
    pthread_rwlock_destroy(&g_files_lock);
    return 0;
}
