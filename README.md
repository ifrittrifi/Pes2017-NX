# PES 2017 — port Nintendo Switch (Pes2017-NX)

Rekompilasi *Pro Evolution Soccer 2017* (Xbox 360, Europe, `default.xex` v0x12, Title ID `0x4A3007D1`)
dari PowerPC ke C++ dengan XenonRecomp, dijalankan sebagai homebrew NRO dengan libnx dan driver Vulkan
Mesa NVK (SDK Horizon dari bo2-360-nx).

Status: **belum playable**. Build NRO dan pengujian di konsol masih berjalan.

## Isi repo

```
ppc/                 C++ hasil rekompilasi default.xex (sudah ada di repo)
runtime/             runtime: kernel/XAM, GPU Xenos -> Vulkan, audio XMA, installer
runtime/switch/      lapisan Horizon: platform (thread, SD), audio audout,
                     guest_memory_nx (memori guest), exception (fault handler + crash log)
switch/source/       input HID
switch/mesa/         stub link untuk Mesa statis
cmake/               toolchain devkitA64
build-switch.sh      build NRO (dipanggil oleh workflow)
third_party/         XenonRecomp, ffmpeg-xenia (XMA), font, lisensi
```

## Model memori di Switch

Diluncurkan lewat hbloader, heap libnx sudah memegang hampir semua memori proses. Karena itu
`svcCreateSharedMemory`/`svcMapPhysicalMemory` gagal dengan `0x10801`. Memori guest sekarang diambil
dari heap (`memalign`), dijadikan code alias (`svcMapProcessCodeMemory`), lalu dicerminkan ke jendela
guest 4 GB (`svcMapProcessMemory`). Bank 512 MB tampil di 0x80000000, 0xA0000000 dan 0xC0000000. Tiap chunk
2 MB baru dipetakan ke sebuah jendela saat jendela itu pertama kali disentuh: fault handler memetakannya
lalu mengulang instruksi. Desain ini dari bo2-360-nx, yang sudah jalan di konsol.

## Menjalankan di Switch

1. Salin `pes2017.nro` ke `sdmc:/switch/pes2017/pes2017.nro`.
2. Salin file game dari disc sendiri ke `sdmc:/switch/pes2017/game/`: `default.xex` (harus SHA-256
   `b914ec9a…79e6`), semua `*.cpk`, dan `nxeart`.
3. Jalankan dalam **mode aplikasi penuh**: tahan **R** saat membuka sebuah game dari HOME Menu sampai
   Homebrew Menu muncul, lalu buka PES 2017. Dari album (mode applet) memorinya hanya sekitar 400 MB.
4. Log ada di `sdmc:/switch/pes2017/pes2017-switch.log`. Kalau crash, ada juga
   `sdmc:/switch/pes2017/crash.log`. Kirim kedua file itu, bersama `pes17.map` dari artifact build yang sama.

## Build

Lewat GitHub Actions (`.github/workflows/build-nro.yml`): container `devkitpro/devkita64` plus SDK Mesa
Horizon dari rilis bo2-360-nx 1.1.0. Hasilnya artifact `pes2017-nro` berisi `pes2017.nro` dan `pes17.map`.
Alamat `image+0x…` di `crash.log` bisa dicari di `pes17.map`, dan fungsi guest bernama `sub_XXXXXXXX`.

Untuk build lokal: `DEVKITPRO=/opt/devkitpro PES17_SWITCH_VULKAN_SDK=<sdk>/switch/portlibs bash build-switch.sh`.

## Kredit

XenonRecomp (hedge-dev), mw2-recompiled (PaulCombal dkk.), Xenia, bo2-360-nx (AlvaroEPena) dan
ReXGlue SDK (Tom Clay; lisensi di `third_party/licenses/bo2-sdk-LICENSE`), mesa-switch (danfromtico),
libnx (switchbrew), FFmpeg. *Pro Evolution Soccer 2017* milik Konami. Repo ini tidak berisi data game.

Lisensi: GPL-3.0-only.
