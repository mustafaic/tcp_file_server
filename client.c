/*
 * client.c - TCP Dosya Sunucusu İstemcisi
 *
 * Kullanım: ./client [sunucu_ip] [port]
 * Varsayılan: 127.0.0.1:8080
 *
 * Protokol (server.c ile eşleşmeli):
 *   LIST              → OK <n>\n  <ad> <boyut>\n … END\n
 *   GET <ad>          → OK <boyut>\n <boyut> ham bayt
 *   PUT <ad> <boyut>  → READY\n  (istemci <boyut> bayt gönderir) → OK\n
 *   DELETE <ad>       → OK\n | ERR\n
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

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define DEFAULT_IP   "127.0.0.1"
#define DEFAULT_PORT 8080
#define BUF_SIZE     (64 * 1024)
#define MAX_FILENAME 256

/* ══════════════════════════════════════════════════════════
 *  SOKET YARDIMCILARI
 * ══════════════════════════════════════════════════════════ */

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

static void sendf(int fd, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int len = vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    if (len < 0) return;
    buf[len++] = '\n';
    send_all(fd, buf, (size_t)len);
}

/* ══════════════════════════════════════════════════════════
 *  İLERLEME ÇUBUĞU
 * ══════════════════════════════════════════════════════════ */
static void print_progress(long long done, long long total, double mbps)
{
    if (total <= 0) return;
    int pct  = (int)((done * 100) / total);
    int bars = pct / 2;
    printf("\r[");
    for (int i = 0; i < 50; i++) printf(i < bars ? "#" : " ");
    printf("] %3d%%  %lld/%lld B  %.2f MB/s ", pct, done, total, mbps);
    fflush(stdout);
}

/* ══════════════════════════════════════════════════════════
 *  KOMUTLAR
 * ══════════════════════════════════════════════════════════ */

static void cmd_list(int fd)
{
    sendf(fd, "LIST");

    char buf[512];
    if (recv_line(fd, buf, sizeof(buf)) <= 0) {
        puts("HATA: Sunucu yanit vermedi");
        return;
    }
    if (strncmp(buf, "OK", 2) != 0) {
        printf("HATA: %s\n", buf + (strncmp(buf, "ERR ", 4) == 0 ? 4 : 0));
        return;
    }

    printf("\n%-44s %14s\n", "Dosya Adi", "Boyut (B)");
    printf("%.60s\n",
           "------------------------------------------------------------");

    int actual = 0;
    long long total_bytes = 0;
    while (recv_line(fd, buf, sizeof(buf)) > 0) {
        if (strcmp(buf, "END") == 0) break;
        char name[256];
        long long sz = 0;
        if (sscanf(buf, "%255s %lld", name, &sz) == 2) {
            printf("%-44s %14lld\n", name, sz);
            total_bytes += sz;
            actual++;
        }
    }
    printf("%.60s\n",
           "------------------------------------------------------------");
    printf("Toplam: %d dosya, %lld bayt\n\n", actual, total_bytes);
}

static void cmd_get(int fd, const char *filename)
{
    sendf(fd, "GET %s", filename);

    char buf[512];
    if (recv_line(fd, buf, sizeof(buf)) <= 0) {
        puts("HATA: Sunucu yanit vermedi");
        return;
    }
    if (strncmp(buf, "OK", 2) != 0) {
        printf("HATA: %s\n", buf + (strncmp(buf, "ERR ", 4) == 0 ? 4 : 0));
        return;
    }

    long long filesize = 0;
    sscanf(buf, "OK %lld", &filesize);

    FILE *fp = fopen(filename, "wb");
    if (!fp) {
        perror("fopen");
        /* Sunucudan gelen veriyi boşa oku (protokol senkronizasyonu) */
        long long drain = filesize;
        char *tmp = malloc(BUF_SIZE);
        if (tmp) {
            while (drain > 0) {
                ssize_t n = recv(fd, tmp, (size_t)(drain < BUF_SIZE ? drain : BUF_SIZE), 0);
                if (n <= 0) break;
                drain -= n;
            }
            free(tmp);
        }
        return;
    }

    char *chunk = malloc(BUF_SIZE);
    if (!chunk) { perror("malloc"); fclose(fp); return; }

    struct timeval start;
    gettimeofday(&start, NULL);
    long long remaining = filesize;
    long long received  = 0;
    int ok = 1;

    while (remaining > 0) {
        size_t to_recv = (size_t)(remaining < BUF_SIZE ? remaining : BUF_SIZE);
        ssize_t n = recv(fd, chunk, to_recv, 0);
        if (n <= 0) { ok = 0; break; }
        if (fwrite(chunk, 1, (size_t)n, fp) != (size_t)n) { ok = 0; break; }
        received  += n;
        remaining -= n;

        struct timeval now; gettimeofday(&now, NULL);
        double sec = (now.tv_sec - start.tv_sec) + (now.tv_usec - start.tv_usec) / 1e6;
        double mbps = (sec > 0) ? (received / sec / 1048576.0) : 0.0;
        print_progress(received, filesize, mbps);
    }

    free(chunk);
    fclose(fp);
    printf("\n");

    if (ok) {
        struct timeval now; gettimeofday(&now, NULL);
        double sec  = (now.tv_sec - start.tv_sec) + (now.tv_usec - start.tv_usec) / 1e6;
        double mbps = (sec > 0) ? (received / sec / 1048576.0) : 0.0;
        printf("'%s' indirildi — %lld B, %.2f s, %.2f MB/s\n",
               filename, received, sec, mbps);
    } else {
        remove(filename);
        printf("HATA: Aktarim kesintiye ugradi (%lld/%lld B)\n",
               received, filesize);
    }
}

static void cmd_put(int fd, const char *filename)
{
    struct stat st;
    if (stat(filename, &st) != 0) {
        perror("stat");
        return;
    }
    if (!S_ISREG(st.st_mode)) {
        printf("HATA: '%s' duzgun dosya degil\n", filename);
        return;
    }
    long long filesize = (long long)st.st_size;

    FILE *fp = fopen(filename, "rb");
    if (!fp) { perror("fopen"); return; }

    sendf(fd, "PUT %s %lld", filename, filesize);

    char buf[512];
    if (recv_line(fd, buf, sizeof(buf)) <= 0) {
        puts("HATA: Sunucu yanit vermedi");
        fclose(fp);
        return;
    }
    if (strcmp(buf, "READY") != 0) {
        printf("HATA: %s\n", buf + (strncmp(buf, "ERR ", 4) == 0 ? 4 : 0));
        fclose(fp);
        return;
    }

    char *chunk = malloc(BUF_SIZE);
    if (!chunk) { perror("malloc"); fclose(fp); return; }

    struct timeval start;
    gettimeofday(&start, NULL);
    long long sent = 0;
    size_t nread;
    int ok = 1;

    while ((nread = fread(chunk, 1, BUF_SIZE, fp)) > 0) {
        if (send_all(fd, chunk, nread) < 0) { ok = 0; break; }
        sent += (long long)nread;

        struct timeval now; gettimeofday(&now, NULL);
        double sec = (now.tv_sec - start.tv_sec) + (now.tv_usec - start.tv_usec) / 1e6;
        double mbps = (sec > 0) ? (sent / sec / 1048576.0) : 0.0;
        print_progress(sent, filesize, mbps);
    }

    free(chunk);
    fclose(fp);
    printf("\n");

    if (!ok) { printf("HATA: Gonderim kesintisi\n"); return; }

    if (recv_line(fd, buf, sizeof(buf)) <= 0) {
        puts("HATA: Sunucu onay vermedi");
        return;
    }

    if (strcmp(buf, "OK") == 0) {
        struct timeval now; gettimeofday(&now, NULL);
        double sec  = (now.tv_sec - start.tv_sec) + (now.tv_usec - start.tv_usec) / 1e6;
        double mbps = (sec > 0) ? (sent / sec / 1048576.0) : 0.0;
        printf("'%s' yuklendi — %lld B, %.2f s, %.2f MB/s\n",
               filename, sent, sec, mbps);
    } else {
        printf("HATA: %s\n", buf + (strncmp(buf, "ERR ", 4) == 0 ? 4 : 0));
    }
}

static void cmd_delete(int fd, const char *filename)
{
    sendf(fd, "DELETE %s", filename);

    char buf[512];
    if (recv_line(fd, buf, sizeof(buf)) <= 0) {
        puts("HATA: Sunucu yanit vermedi");
        return;
    }
    if (strcmp(buf, "OK") == 0)
        printf("'%s' silindi\n", filename);
    else
        printf("HATA: %s\n", buf + (strncmp(buf, "ERR ", 4) == 0 ? 4 : 0));
}

/* ══════════════════════════════════════════════════════════
 *  YARDIM MESAJI
 * ══════════════════════════════════════════════════════════ */
static void print_help(void)
{
    puts("Komutlar:");
    puts("  list               — sunucudaki dosyalari listele");
    puts("  get  <dosya>       — sunucudan dosya indir");
    puts("  put  <dosya>       — sunucuya dosya yukle");
    puts("  delete <dosya>     — sunucudaki dosyayi sil");
    puts("  help               — bu yardim metnini goster");
    puts("  quit               — cikis\n");
}

/* ══════════════════════════════════════════════════════════
 *  ANA FONKSİYON
 * ══════════════════════════════════════════════════════════ */
int main(int argc, char *argv[])
{
    signal(SIGPIPE, SIG_IGN);

    const char *server_ip   = DEFAULT_IP;
    int         server_port = DEFAULT_PORT;

    if (argc >= 2) server_ip   = argv[1];
    if (argc >= 3) server_port = atoi(argv[2]);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(server_port);

    if (inet_pton(AF_INET, server_ip, &addr.sin_addr) <= 0) {
        fprintf(stderr, "Gecersiz IP adresi: %s\n", server_ip);
        return 1;
    }

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        return 1;
    }

    /* Hoşgeldin mesajı */
    char buf[512];
    recv_line(fd, buf, sizeof(buf));
    printf("\n%s\n", buf);
    print_help();

    char line[512];
    while (1) {
        printf("> ");
        fflush(stdout);

        if (!fgets(line, sizeof(line), stdin)) break;
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] == '\0') continue;

        char cmd[64]             = {0};
        char arg[MAX_FILENAME]   = {0};
        sscanf(line, "%63s %255s", cmd, arg);

        if (strcasecmp(cmd, "quit") == 0 ||
            strcasecmp(cmd, "exit") == 0) {
            sendf(fd, "QUIT");
            recv_line(fd, buf, sizeof(buf));
            printf("%s\n", buf);
            break;

        } else if (strcasecmp(cmd, "list") == 0) {
            cmd_list(fd);

        } else if (strcasecmp(cmd, "get") == 0) {
            if (arg[0] == '\0') { puts("Kullanim: get <dosya_adi>"); continue; }
            cmd_get(fd, arg);

        } else if (strcasecmp(cmd, "put") == 0) {
            if (arg[0] == '\0') { puts("Kullanim: put <dosya_adi>"); continue; }
            cmd_put(fd, arg);

        } else if (strcasecmp(cmd, "delete") == 0) {
            if (arg[0] == '\0') { puts("Kullanim: delete <dosya_adi>"); continue; }
            cmd_delete(fd, arg);

        } else if (strcasecmp(cmd, "help") == 0) {
            print_help();

        } else {
            printf("Bilinmeyen komut: '%s'  (help yazin)\n", cmd);
        }
    }

    close(fd);
    return 0;
}
