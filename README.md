# TES-0D-02-01 Compatible DC Charger Controller for ESP32-S3

# ⚡ TES-EVSE Controller (TES 協議控制核心)

> 專為 TES-0D-02-01 標準設計的開源電動機車直流充電控制器。
> 基於 ESP32-S3 架構，支援 Web UI 監控與 OTA 更新。

[![License: CC BY-NC-SA 4.0](https://img.shields.io/badge/License-CC%20BY--NC--SA%204.0-lightgrey.svg)](https://creativecommons.org/licenses/by-nc-sa/4.0/deed.zh_TW)
[![Platform](https://img.shields.io/badge/platform-ESP32--S3-orange.svg)](https://www.espressif.com/)
[![Framework](https://img.shields.io/badge/framework-PlatformIO-blue.svg)](https://platformio.org/)

---

## ⚠️ 免責聲明與安全警告 (Disclaimer & Safety)

**本專案僅包含「控制板」的軟硬體設計，不包含功率級電源模組。**

1.  **高壓危險**：本控制器需配合高壓直流電源使用，組裝與測試過程存在觸電與火災風險。
2.  **非商業產品**：本專案為個人研究與技術驗證性質，未經 BSMI、UL 等安規認證。
3.  **責任歸屬**：使用本專案所產生的任何後果（包括但不限於車輛損壞、電池故障、人身傷害），**使用者需自行承擔**。若不具備相關電學知識，請勿嘗試製作。
4.  **BMS 風險**：CAN Bus 線路若接線錯誤或短路，**將導致車輛 BMS (電池管理系統) 永久性損壞**，請務必在低壓環境確認無誤後再上機。

---

## 🎯 專案範圍 (Scope)

本 Repository 專注於 **TES 協議控制邏輯 (Protocol Logic)** 的實作。
*   **包含**：ESP32-S3 控制韌體、CAN Bus 通訊電路設計、狀態機邏輯。
*   **不包含**：功率級整流器 (Rectifier)、AC/DC 電源模組的控制碼 (請依據您選用的電源模組自行適配)。

---

## ✨ 功能特點 (Features)

### 核心協議
*   **TES 標準相容**: 完整實作 **TES-0D-02-01** 充電通訊協議。
*   **寬範圍支援**: 協議邏輯最高支援 **120V / 100A (12kW)** 輸出能力。
    *   *註：實際輸出能力取決於您搭配的電源模組與線徑。*
*   **通用性**: 適用於 eMoving iE125 等支援 TES 快充標準之車輛。

### 智慧功能
*   **Web UI**: 內建網頁伺服器，可透過手機設定電流、截止電壓、目標 SOC。
*   **OTA 更新**: 支援無線韌體升級，方便後續功能維護。

---

## 🛠️ 硬體設計 (Hardware Design)

本專案硬體設計以 **通用性** 為核心，開發者可依據需求選擇實作方式。

*   **Schematic (原理圖)**: 請參閱 `docs/PCB/` 目錄下的檔案。這是核心電路設計，包含了 MCU 接腳定義、CAN Transceiver 線路與周邊控制電路。
*   **BOM (元件清單)**: 請參閱 `docs/` 目錄下的檔案。主要採用通用型電子元件（如 ESP32-S3 開發板、繼電器模組等），方便開發者自行取得。
*   **Reference PCB (參考佈線)**: 目錄中亦提供了一份已驗證的 PCB 設計檔（Gerber）作為參考實作，供有需要的開發者研究佈線邏輯。

> **實作建議**：本設計亦適合使用 **萬用板 (Perfboard)** 進行手工搭建。請依據原理圖連接對應線路即可達到相同功能。

---

## 💻 軟體開發 (Development)

本專案使用 **PlatformIO** 進行開發。

1.  **環境建置**: 安裝 VS Code + PlatformIO 外掛。
2.  **硬體適配**:
    *   本程式碼具備高度可配置性。若您使用自製電路板（如洞洞板），請務必在 `src/config.h` 中修改 **Pin Definitions (引腳定義)** 以符合您的實際接線。
3.  **參數設定**:
    *   OLED I2C 地址 (預設 0x3C / 0x3D)
    *   電壓/電流校正參數 (ADC Calibration)

---

## 🕹️ 操作說明 (Operation)

*   **進入設定選單**: 待機狀態下，長按 **Setting** 按鈕 1~2 秒。
*   **選單操作**:
    *   `Start`: 上一項 / 增加數值
    *   `Stop`: 下一項 / 減少數值
    *   `Setting`: 確認 / 進入

---

## 🤝 社群與支援 (Community)

歡迎加入社群討論改裝心得、回報 Bug 或分享您的實作案例。

*   **Facebook 社群**: [TES 電動機車充電技術交流](https://www.facebook.com/groups/791962053528872/?ref=share&mibextid=NSMWBT)

---

## ⚖️ 授權 (License)

本專案採用 **[CC BY-NC-SA 4.0](https://creativecommons.org/licenses/by-nc-sa/4.0/deed.zh_TW)** 授權。

*   ✅ **分享與改作**：需標示原作者，並以相同條款釋出。
*   🚫 **禁止商業使用**：**不得將本專案之設計圖檔、程式碼用於商業量產或販售營利。**

Copyright (c) 2025 Chris Huang

## 影片(Video)
https://youtube.com/shorts/SKAtfQcCqX8?si=aqei7ZD7hVCWWM0R

https://youtu.be/vA7gSdK1YZQ?si=lSQAtU0p7vCutx1Y

## 照片(Images)

*   **此為專案V2早期時所拍的照片，本人V2後期轉為自行畫PCB並且使用ESP32-S3開發**

![](docs/images/20250804_205003.jpg)
![](docs/images/20250804_205006.jpg)
![](docs/images/20250804_205017.jpg)
![](docs/images/20250804_205024.jpg)
![](docs/images/20250804_205031.jpg)
![](docs/images/20250804_205036.jpg)
![](docs/images/20250804_205042.jpg)
![](docs/images/20250804_205055.jpg)
![](docs/images/20250804_205058.jpg)
![](docs/images/20250804_205101.jpg)
![](docs/images/20250804_205114.jpg)
![](docs/images/20250804_205116.jpg)
