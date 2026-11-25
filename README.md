# PES 2017 — port Nintendo Switch (Pes2017-NX)

Rekompilasi *Pro Evolution Soccer 2017* (Xbox 360, Europe, `default.xex` v0x12, Title ID `0x4A3007D1`)
dari PowerPC ke C++ dengan XenonRecomp, dijalankan sebagai homebrew NRO dengan libnx dan driver Vulkan
Mesa NVK (SDK Horizon dari bo2-360-nx).

Status: **bisa dimainkan** di konsol (menu ±60 fps; pertandingan ±26 gambar per detik dengan kecepatan
game ±85–90%; entrance dan selebrasi masih lebih berat). Pekerjaan sekarang: performa di pertandingan,
entrance dan selebrasi. Belum ada: ganti papan skor, sembunyikan radar, dan online.

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
   `b914ec9a…79e6`, versi 0x12), semua `*.cpk`, dan `nxeart`. Title Update (`default.xexp`, ke versi 0x612)
   **belum** didukung dan tidak perlu disalin.
3. Jalankan dalam **mode aplikasi penuh**: tahan **R** saat membuka sebuah game dari HOME Menu sampai
   Homebrew Menu muncul, lalu buka PES 2017. Dari album (mode applet) memorinya hanya sekitar 400 MB.
4. Log ada di `sdmc:/switch/pes2017/pes2017-switch.log`. Kalau crash, ada juga
   `sdmc:/switch/pes2017/crash.log`. Kirim kedua file itu, bersama `pes17.map` dari artifact build yang sama.

Clock CPU/GPU/memori diatur dari dalam game (tanpa sys-clk): CPU 1785, GPU 614 (768 docked), memori 1600 MHz.

## Pengaturan: `sdmc:/switch/pes2017/config.ini`

Dibuat otomatis kalau belum ada, berisi semua opsi (bagian `[log]`, `[graphics]`, `[clocks]`,
`[performance]`, `[shader]`). Kunci huruf kecil, nilai `true`/`false`; baris yang diawali `;` atau `#` diabaikan.
Yang paling sering dipakai:

| Kunci | Fungsi |
|---|---|
| `pes17_report` / `pes17_gpu_timing` | log perf dan waktu GPU per pass (untuk kirim log) |
| `pes17_scene_scale` | skala scene (0.8 sekarang); FSR menaikkan ke layar |
| `pes17_no_crowd` / `pes17_crowd_density` / `pes17_crowd_adaptive` | matikan penonton, atau kepadatan 0.25–1 (default 0.5); adaptif menipiskan penonton otomatis saat GPU tertinggal dan mengembalikannya saat lancar |
| `pes17_shadow_strength` | gelap bayangan pemain |
| `pes17_spike_ms` / `pes17_auto_dump` | catat lonjakan (ms antar swap) dengan penyebabnya; dump frame otomatis (maks 6) saat beberapa frame berturut-turut lambat |
| `pes17_frame_skip` | lompat frame bergantian saat GPU tertinggal (mengurangi slow motion) |
| `pes17_cpu_mhz` / `pes17_gpu_mhz` / `pes17_mem_mhz`, `pes17_clocks` | clock dari dalam game |

Cache shader dan pipeline ada di `sdmc:/switch/pes2017/shader/` (`shader_cache.bin`, `pipeline_cache.bin`).
Jangan dihapus: kunjungan pertama ke tiap tim membuat shader baru, dan glitch hitam di kit pemain berkurang
seiring cache terisi.

## Tombol diagnostik

- **L3 + R3** (kedua stik ditekan) saat adegan tampil: dump satu frame (daftar draw dan gambar) ke folder `dump`.
  Paling banyak satu per 10 detik. Kirim bersama log untuk analisis.
- **ZL + ZR + Minus**: hanya di build pelatihan PGO, menulis profil ke `sdmc:/switch/pes2017/pgo/`.

## Build

Lewat GitHub Actions, semuanya manual (Actions → pilih workflow → Run workflow), tidak ada build otomatis:

- **Build Switch NRO** (`build-nro-skip-cache.yml`): build cepat yang memakai cache objek PPC.
- **Build Switch NRO (full rebuild, manual)** (`build-nro.yml`): rebuild penuh, dengan pilihan
  `pgo` (`none`, `generate`, `use`) dan `lto`. Lihat `docs/pgo.md`.
- **Apply source ZIP**: berjalan sendiri saat `pes2017-nx-src.zip` di-push, membuka isinya ke repo.

Container `devkitpro/devkita64` plus SDK Mesa Horizon dari rilis bo2-360-nx 1.1.0. Hasilnya artifact
`pes2017-nro` berisi `pes2017.nro` dan `pes17.map`. Alamat `image+0x…` di `crash.log` bisa dicari di
`pes17.map`, dan fungsi guest bernama `sub_XXXXXXXX`.

Untuk build lokal: `DEVKITPRO=/opt/devkitpro PES17_SWITCH_VULKAN_SDK=<sdk>/switch/portlibs bash build-switch.sh`.

## Catatan performa

Beban terbesar di pertandingan: pass scene HDR (±9–10 ms), lalu penonton (±7 ms bila menyala) dan bayangan
pemain. Di selebrasi, pass bayangan depth 1040x1024 dan shader yang baru pertama dipakai. CPU tidak jenuh
(±1,2 dari 4 core); batasnya rantai tunggu GPU–CPU. Pekerjaan berikutnya: PGO untuk kode PPC.

## Kredit

XenonRecomp (hedge-dev), mw2-recompiled (PaulCombal dkk.), Xenia, bo2-360-nx (AlvaroEPena) dan
ReXGlue SDK (Tom Clay; lisensi di `third_party/licenses/bo2-sdk-LICENSE`), mesa-switch (danfromtico),
libnx (switchbrew), FFmpeg. *Pro Evolution Soccer 2017* milik Konami. Repo ini tidak berisi data game.

Lisensi: GPL-3.0-only.
