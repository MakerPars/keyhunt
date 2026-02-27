#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
KeyHunt Otomatik Döngü Scripti (Argüman Destekli)
==================================================
Kullanım örnekleri:
  python3 keyhunt_runner.py
  python3 keyhunt_runner.py --duration 10 --token "BOT_TOKEN" --chat-id 123456
  python3 keyhunt_runner.py --no-gpu --threads 4 --min 500000000000000000
  python3 keyhunt_runner.py -i 0,1 -x 256,128,256,128
  python3 keyhunt_runner.py --help
"""

import os
import sys
import time
import random
import logging
import argparse
import subprocess
import threading
import requests
from datetime import datetime

# ============================================================
#            ⚙️  VARSAYILAN PARAMETRELER (Default Values)
#            Argüman girilmezse bunlar kullanılır.
# ============================================================

DEFAULTS = {
    "keyhunt_path"     : "./KeyHunt",
    "threads"          : 0,                       # 0 = otomatik (tüm core'lar)
    "gpu"              : True,                    # -g bayrağı
    "gpu_ids"          : None,                    # Örn: "0,1"
    "gpu_grid"         : None,                    # Örn: "256,128,256,128"
    "address"          : "1PWo3JeB9jrGwfHDNpdGK54CRas7fsVzXU",
    "range_min"        : "400000000000000000",    # Rastgele start alt sınırı
    "range_max"        : "7fffffffffffffffff",    # End değeri / üst sınır
    "duration"         : 15,                      # Dakika cinsinden döngü süresi
    "found_file"       : "Found.txt",
    "check_interval"   : 5,                       # Found.txt kontrol aralığı (sn)
    "telegram_token"   : "TEST:TEST",     # Telegram bot token
    "telegram_chat_id" : 123123321,               # Telegram chat ID
}

# ============================================================
#                     ARGÜMAN PARSER
# ============================================================

def parse_args():
    parser = argparse.ArgumentParser(
        description="KeyHunt Otomatik Döngü Scripti",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )

    # --- KeyHunt Yolu ---
    parser.add_argument(
        "--keyhunt", metavar="PATH",
        default=DEFAULTS["keyhunt_path"],
        help="KeyHunt çalıştırılabilir dosyasının yolu",
    )

    # --- CPU Thread ---
    parser.add_argument(
        "-t", "--threads", metavar="N", type=int,
        default=DEFAULTS["threads"],
        help="CPU thread sayısı (0 = otomatik / tüm core'lar)",
    )

    # --- GPU Aç/Kapat ---
    gpu_group = parser.add_mutually_exclusive_group()
    gpu_group.add_argument(
        "-g", "--gpu", dest="gpu", action="store_true",
        default=DEFAULTS["gpu"],
        help="GPU hesaplamasını etkinleştir",
    )
    gpu_group.add_argument(
        "--no-gpu", dest="gpu", action="store_false",
        help="GPU hesaplamasını devre dışı bırak",
    )

    # --- GPU ID'leri ---
    parser.add_argument(
        "-i", "--gpu-ids", metavar="IDS",
        default=DEFAULTS["gpu_ids"],
        dest="gpu_ids",
        help="Kullanılacak GPU id'leri, örn: 0,1",
    )

    # --- GPU Grid ---
    parser.add_argument(
        "-x", "--gpu-grid", metavar="GRID",
        default=DEFAULTS["gpu_grid"],
        dest="gpu_grid",
        help="GPU kernel grid boyutu, örn: 256,128 veya 256,128,256,128",
    )

    # --- Bitcoin Adresi ---
    parser.add_argument(
        "-a", "--address", metavar="ADDR",
        default=DEFAULTS["address"],
        help="Hedef Bitcoin adresi",
    )

    # --- Aralık ---
    parser.add_argument(
        "--min", metavar="HEX",
        default=DEFAULTS["range_min"],
        help="Rastgele start için alt sınır (hex, 0x prefix olmadan)",
    )
    parser.add_argument(
        "--max", metavar="HEX",
        default=DEFAULTS["range_max"],
        help="Arama üst sınırı / end değeri (hex, 0x prefix olmadan)",
    )

    # --- Döngü Süresi ---
    parser.add_argument(
        "--duration", metavar="MINUTES", type=int,
        default=DEFAULTS["duration"],
        help="Her döngüde KeyHunt'ın çalışacağı süre (dakika)",
    )

    # --- Found Dosyası ---
    parser.add_argument(
        "--found-file", metavar="PATH",
        default=DEFAULTS["found_file"],
        dest="found_file",
        help="İzlenecek sonuç dosyasının adı/yolu",
    )

    # --- Kontrol Aralığı ---
    parser.add_argument(
        "--check-interval", metavar="SEC", type=int,
        default=DEFAULTS["check_interval"],
        dest="check_interval",
        help="Found dosyası kontrol aralığı (saniye)",
    )

    # --- Telegram ---
    parser.add_argument(
        "--token", metavar="TOKEN",
        default=DEFAULTS["telegram_token"],
        help="Telegram bot token'ı",
    )
    parser.add_argument(
        "--chat-id", metavar="ID", type=int,
        default=DEFAULTS["telegram_chat_id"],
        dest="chat_id",
        help="Telegram chat ID",
    )

    return parser.parse_args()

# ============================================================
#                     LOG AYARLARI
# ============================================================

def setup_logging():
    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s [%(levelname)s] %(message)s",
        datefmt="%Y-%m-%d %H:%M:%S",
        handlers=[
            logging.FileHandler("keyhunt_runner.log", encoding="utf-8"),
            logging.StreamHandler(sys.stdout),
        ],
    )
    return logging.getLogger(__name__)

# ============================================================
#                     TELEGRAM GÖNDERİM
# ============================================================

def send_to_telegram(token: str, chat_id: int, file_path: str, log) -> bool:
    bad_tokens = {"ASFASASD:ASDASDA", "BURAYA", ""}
    if not token or any(t in token for t in bad_tokens):
        log.warning("⚠️  Telegram token ayarlanmamış, dosya gönderilemedi.")
        return False

    if not os.path.exists(file_path):
        log.error(f"❌ Gönderilecek dosya bulunamadı: {file_path}")
        return False

    url     = f"https://api.telegram.org/bot{token}/sendDocument"
    caption = (
        f"🎉 KeyHunt - Found.txt bulundu!\n"
        f"📅 {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}"
    )
    try:
        with open(file_path, "rb") as f:
            response = requests.post(
                url,
                data={"chat_id": chat_id, "caption": caption},
                files={"document": f},
                timeout=30,
            )
        if response.status_code != 200:
            log.error(f"❌ HTTP Hatası: {response.status_code} → {response.text}")
            return False
        result = response.json()
        if not result.get("ok"):
            log.error(f"❌ Telegram API Hatası: {result}")
            return False
        log.info("✅ Found.txt başarıyla Telegram'a gönderildi!")
        return True
    except requests.exceptions.Timeout:
        log.error("❌ Telegram: Bağlantı zaman aşımına uğradı.")
    except requests.exceptions.ConnectionError:
        log.error("❌ Telegram: İnternet bağlantı hatası.")
    except Exception as e:
        log.error(f"❌ Telegram: Beklenmeyen hata → {e}")
    return False

# ============================================================
#                  FOUND.TXT İZLEYİCİ (Thread)
# ============================================================

_found_sent = False

def found_watcher(args, stop_event: threading.Event, log):
    global _found_sent
    log.info(f"👁️  Found.txt izleyici başlatıldı → '{args.found_file}' izleniyor "
             f"(kontrol aralığı: {args.check_interval}s)")
    while not stop_event.is_set():
        if not _found_sent and os.path.exists(args.found_file):
            size = os.path.getsize(args.found_file)
            if size > 0:
                log.info(f"🎯 Found.txt tespit edildi! ({size} byte) → Telegram'a gönderiliyor...")
                if send_to_telegram(args.token, args.chat_id, args.found_file, log):
                    _found_sent = True
                    log.info("🏁 Dosya gönderildi, izleyici duruyor.")
                    stop_event.set()
                    break
                else:
                    log.warning("⚠️  Gönderim başarısız, tekrar denenecek...")
        time.sleep(args.check_interval)
    log.info("👁️  Found.txt izleyici durduruldu.")

# ============================================================
#                  YARDIMCI FONKSİYONLAR
# ============================================================

def random_hex_in_range(min_hex: str, max_hex: str) -> str:
    min_val  = int(min_hex, 16)
    max_val  = int(max_hex, 16)
    rand_val = random.randint(min_val, max_val)
    return format(rand_val, "x")

def build_command(args, start_hex: str, end_hex: str) -> list:
    cmd = [args.keyhunt]
    cmd += ["-t", str(args.threads)]
    if args.gpu:
        cmd.append("-g")
    if args.gpu_ids:
        cmd += ["-i", args.gpu_ids]
    if args.gpu_grid:
        cmd += ["-x", args.gpu_grid]
    cmd += ["-s", start_hex, "-e", end_hex]
    cmd += ["-a", args.address]
    return cmd

def kill_process(process, log):
    if process.poll() is not None:
        log.info("ℹ️  KeyHunt zaten sonlanmış.")
        return
    log.info(f"⏹️  KeyHunt (PID: {process.pid}) sonlandırılıyor...")
    try:
        process.terminate()
        try:
            process.wait(timeout=10)
            log.info("✅ KeyHunt düzgünce sonlandırıldı (SIGTERM).")
        except subprocess.TimeoutExpired:
            log.warning("⚠️  SIGTERM'e yanıt vermedi, SIGKILL gönderiliyor...")
            process.kill()
            process.wait()
            log.info("✅ KeyHunt zorla durduruldu (SIGKILL).")
    except Exception as e:
        log.error(f"❌ KeyHunt sonlandırılırken hata: {e}")

# ============================================================
#                      ANA DÖNGÜ
# ============================================================

def main():
    args         = parse_args()
    log          = setup_logging()
    duration_sec = args.duration * 60

    log.info("=" * 65)
    log.info("🚀 KeyHunt Otomatik Döngü Scripti Başlatıldı")
    log.info(f"   KeyHunt      : {args.keyhunt}")
    log.info(f"   Adres        : {args.address}")
    log.info(f"   Aralık       : {args.min}  →  {args.max}")
    log.info(f"   GPU          : {'Açık' if args.gpu else 'Kapalı'}")
    if args.gpu and args.gpu_ids:
        log.info(f"   GPU ID'ler   : {args.gpu_ids}")
    if args.gpu and args.gpu_grid:
        log.info(f"   GPU Grid     : {args.gpu_grid}")
    log.info(f"   CPU Thread   : {args.threads if args.threads > 0 else 'Otomatik'}")
    log.info(f"   Döngü Süresi : {args.duration} dakika ({duration_sec} saniye)")
    log.info(f"   Found Dosya  : {args.found_file}")
    log.info(f"   Telegram     : {'Ayarlı ✅' if args.token not in ('ASFASASD:ASDASDA','BURAYA','') else 'Ayarsız ⚠️'}")
    log.info("=" * 65)

    stop_event     = threading.Event()
    watcher_thread = threading.Thread(
        target=found_watcher, args=(args, stop_event, log), daemon=True
    )
    watcher_thread.start()

    cycle = 0
    try:
        while not stop_event.is_set():
            cycle    += 1
            start_hex = random_hex_in_range(args.min, args.max)
            end_hex   = args.max
            cmd       = build_command(args, start_hex, end_hex)

            log.info("-" * 65)
            log.info(f"🔄 Döngü #{cycle} başlıyor")
            log.info(f"   Start : {start_hex}")
            log.info(f"   End   : {end_hex}")
            log.info(f"   Komut : {' '.join(cmd)}")

            try:
                process = subprocess.Popen(
                    cmd,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT,
                    text=True,
                    bufsize=1,
                )
            except FileNotFoundError:
                log.critical(f"❌ KeyHunt bulunamadı: {args.keyhunt}  →  Script durduruluyor.")
                stop_event.set()
                break
            except PermissionError:
                log.critical(f"❌ Çalıştırma izni yok: {args.keyhunt}  →  Script durduruluyor.")
                stop_event.set()
                break

            log.info(f"✅ KeyHunt başlatıldı (PID: {process.pid})")

            def log_output(proc):
                for line in proc.stdout:
                    line = line.rstrip()
                    if line:
                        log.info(f"[KeyHunt] {line}")

            output_thread = threading.Thread(target=log_output, args=(process,), daemon=True)
            output_thread.start()

            # Süre dolana veya stop sinyali gelene kadar bekle
            deadline = time.time() + duration_sec
            while time.time() < deadline and not stop_event.is_set():
                if process.poll() is not None:
                    log.warning("⚠️  KeyHunt beklenmedik şekilde erken sonlandı.")
                    break
                time.sleep(1)

            kill_process(process, log)
            output_thread.join(timeout=5)

            if stop_event.is_set():
                log.info("🛑 Stop sinyali alındı, döngü sonlandırılıyor.")
                break

            log.info(f"✅ Döngü #{cycle} tamamlandı. Yeni döngü başlatılıyor...\n")

    except KeyboardInterrupt:
        log.info("\n⛔ Kullanıcı tarafından durduruldu (Ctrl+C).")
        stop_event.set()

    finally:
        stop_event.set()
        watcher_thread.join(timeout=10)
        log.info("=" * 65)
        log.info(f"🏁 Script sonlandı. Toplam tamamlanan döngü: {cycle}")
        log.info("=" * 65)

# ============================================================
if __name__ == "__main__":
    main()