# Çok İstemcili TCP Dosya Sunucusu

## Amaç

Bu proje, C ve POSIX/Linux API'si kullanılarak geliştirilmiş, TCP soketleri üzerinden çalışan çok istemcili bir dosya transfer sunucusudur. İstemciler sunucuya bağlanarak `LIST`, `GET`, `PUT`, `DELETE` komutlarıyla dosya listeleyebilir, indirebilir, yükleyebilir ve silebilir.

---

## Tasarım

### Mimari: Thread-per-Client

Her gelen bağlantı için `pthread_create()` ile ayrı bir thread oluşturulur. Thread, işi bitince `pthread_detach()` ile bağımsız hale getirilir.

```
[İstemci 1] ──┐
[İstemci 2] ──┼──► accept() ──► pthread_create() ──► connection_handler()
[İstemci 3] ──┘
```

### Protokol: Uzunluk-Önekli Metin Protokolü

Eski EOF-tabanlı protokolün TCP parçalanması karşısındaki kırılganlığı giderilmiştir. Yeni protokolde dosya boyutu komutla birlikte gönderilir; alıcı taraf tam olarak bu kadar baytı okur.

| Komut | İstemci Gönderir | Sunucu Yanıtı |
|-------|-----------------|---------------|
| `LIST` | `LIST\n` | `OK <n>\n` sonra `<ad> <boyut>\n` satırları, `END\n` |
| `GET <ad>` | `GET dosya.txt\n` | `OK <boyut>\n` + `<boyut>` ham bayt veya `ERR <mesaj>\n` |
| `PUT <ad> <boyut>` | `PUT dosya.txt 1024\n` + baytlar | `READY\n` → (veri) → `OK\n` |
| `DELETE <ad>` | `DELETE dosya.txt\n` | `OK\n` veya `ERR <mesaj>\n` |
| `QUIT` | `QUIT\n` | `BYE\n` |

### Güvenlik

- **Path Traversal Engeli:** Dosya adında `..`, `/`, `\` veya kontrol karakteri varsa istek reddedilir.
- **Dosya Boyutu Sınırı:** PUT işleminde maksimum 512 MB kontrolü yapılır.
- **İstemci Limiti:** Eşzamanlı maksimum 50 istemci kabul edilir.
- **Bağlantı Timeout:** 120 saniye inaktivite sonrasında bağlantı otomatik kesilir.
- **Kısmi Dosya Temizliği:** Kesilen PUT işlemlerinde yarım kalan dosya sunucu tarafından silinir.

### Dosya Yapısı

```
.
├── server.c      — sunucu uygulaması
├── client.c      — istemci uygulaması
├── Makefile      — derleme yapılandırması
├── README.md     — bu dosya
└── storage/      — sunucu tarafında oluşturulan depolama dizini
```

---

## Kullanılan Sistem Programlama Kavramları

### Thread'ler (Eşzamanlılık)
`pthread_create()` ile her istemci için bağımsız thread oluşturulur. Thread'ler `pthread_detach()` ile serbest bırakılır; böylece `pthread_join()` beklenmeksizin bellek otomatik temizlenir.

### Senkronizasyon Mekanizmaları
Projede **üç farklı** senkronizasyon mekanizması kullanılmaktadır:

| Mekanizma | Değişken | Amaç |
|-----------|----------|------|
| `pthread_mutex_t` | `g_log_mutex` | Log dosyasına seri erişim — eşzamanlı yazışmada satır karışmasını önler |
| `pthread_rwlock_t` | `g_files_lock` | Dosya sistemi operasyonları — GET/LIST okuma kilidiyle eşzamanlı çalışır; PUT/DELETE yazma kilidiyle münhasır erişim sağlar |
| `pthread_mutex_t` | `g_cnt_mutex` | Aktif istemci sayacının atomik güncellenmesi |

`pthread_rwlock_t` kullanımı sayesinde çok sayıda istemci aynı anda dosya indirebilir (okuma kilitleri çakışmaz). Yükleme sırasında ise diğer yazma işlemleri beklemeye alınır.

### Socket Programlama
`socket()`, `bind()`, `listen()`, `accept()`, `recv()`, `send()` çağrıları kullanılmıştır. `SO_REUSEADDR` ile hızlı yeniden başlatma desteği, `SO_RCVTIMEO` ile istemci zaman aşımı sağlanmıştır. `MSG_NOSIGNAL` bayrağı bağlantı koptuğunda `SIGPIPE` üretilmesini engeller.

### Dosya G/Ç
`fread()`/`fwrite()` ile 64 KB'lık bloklar halinde aktarım yapılır. Büyük dosyalarda `malloc()` ile yığında (heap) tek seferlik tampon ayrılır, döngü boyunca yeniden kullanılır.

### Sinyal Yönetimi
`SIGINT` ve `SIGTERM` sinyalleri `sigaction()` ile yakalanır. İşleyici `g_running = 0` bayrağını setler ve sunucu soketi kapatarak `accept()` döngüsünü kırar; böylece düzgün kapatma sağlanır.

### Hata Yönetimi
- Tüm sistem çağrılarının dönüş değerleri kontrol edilir.
- Hata durumlarında `perror()` veya `strerror(errno)` ile açıklayıcı mesaj üretilir.
- Kesilen PUT işlemlerinde kısmi dosya sunucu tarafından `unlink()` ile silinir.
- Hatalı komutlar sunucuyu çökertmez; istemciye `ERR <açıklama>` yanıtı döner.

---

## Çalıştırma Adımları

```bash
# 1. Derleme
make

# 2. Sunucuyu başlat (ayrı terminal)
./server

# 3. İstemciyi bağla (yeni terminal)
./client                        # varsayılan: 127.0.0.1:8080
./client 192.168.1.10           # farklı IP
./client 192.168.1.10 9090      # farklı IP ve port

# 4. İstemci komutları
> list
> put buyuk_dosya.bin
> get buyuk_dosya.bin
> delete buyuk_dosya.bin
> quit

# 5. Sunucuyu kapat (Ctrl+C veya kill)
# Temizleme
make clean
```

---

## Testler

### 1. Temel İşlevsellik Testi

```bash
# Test dosyası oluştur
dd if=/dev/urandom of=test_50mb.bin bs=1M count=50

# Yükleme
> put test_50mb.bin
# Beklenen çıktı:
# [####...###] 100%  52428800/52428800 B  xxx.xx MB/s
# 'test_50mb.bin' yuklendi — 52428800 B, x.xx s, xx.xx MB/s

# İndirme
> get test_50mb.bin

# Bütünlük kontrolü
md5sum test_50mb.bin storage/test_50mb.bin
# Her iki hash aynı olmalı
```

### 2. Path Traversal Güvenlik Testi

```bash
> get ../server.c
# Beklenen: HATA: Gecersiz dosya adi

> get ../../etc/passwd
# Beklenen: HATA: Gecersiz dosya adi

> put /tmp/evil.sh 0
# Beklenen: HATA: Gecersiz dosya adi
```

### 3. Eşzamanlı İstemci Testi

```bash
# 5 ayrı terminalde eşzamanlı olarak:
for i in $(seq 1 5); do
  (./client 127.0.0.1 8080 <<'EOF'
put test_50mb.bin
quit
EOF
  ) &
done
wait
# Server log'u 5 ayrı bağlantıyı ve aktarım hızlarını göstermeli
```

### 4. Hatalı İstek Testi

```bash
# Var olmayan dosya
> get yokboylesibir.txt
# Beklenen: HATA: Dosya bulunamadi

# Eksik argüman
> get
# Beklenen: Kullanim: get <dosya_adi>

# Bilinmeyen komut
> XYZ
# Beklenen: Bilinmeyen komut
```

---

## Performans Değerlendirmesi

Her `GET` ve `PUT` işlemi sunucu logunda aktarım hızını raporlar:

```
[2024-06-03 14:22:10] [INFO ] [127.0.0.1:54321] PUT 'test_50mb.bin': 52428800 B, 1.23 s, 40.74 MB/s
[2024-06-03 14:22:15] [INFO ] [127.0.0.1:54322] GET 'test_50mb.bin': 52428800 B, 0.89 s, 56.32 MB/s
```

**Tampon büyüklüğünün etkisi:** 64 KB'lık bloklar, loopback arabirimi üzerinde ~500–900 MB/s'e ulaşabilir. 1 KB tamponla bu değer 10 kata kadar düşer çünkü sistem çağrısı sayısı artar.

**Thread sayısına göre:**  Eşzamanlı istemci sayısı arttıkça her bağlantının bant genişliği, ağ ve disk G/Ç kapasitesine göre orantılı biçimde paylaşılır. Sunucu, `pthread_rwlock_t` sayesinde çoklu GET işlemlerinde bloklanmaz; tek bir PUT ise tüm okuma işlemlerini kısa süreliğine bekletir.

---

