# TES-0D-02-01 Compatible DC Charger Controller for ESP32

[![License: CC BY-NC-SA 4.0](https://img.shields.io/badge/License-CC%20BY--NC--SA%204.0-lightgrey.svg)](https://creativecommons.org/licenses/by-nc-sa/4.0/)

A DIY, open-source DC charger controller project for electric scooters, compatible with the TES-0D-02-01 industry standard, built upon the ESP32 platform.

一個基於ESP32開發的DIY開源電動機車直流充電樁控制器，相容TES-0D-02-01產業標準。

**禁止商業使用**

---

## ⚠️ 安全警告 (Safety Warning) ⚠️

**本專案僅為個人學習、研究和技術驗證目的而創建，並非商業級產品。**

充電樁是涉及高電壓、大電流的設備，直接關係到人身安全和財產（車輛、電網）安全。本專案提供的軟體和硬體設計思路**未經過任何形式的專業安全認證**（如UL、CE、BSMI等）。

**使用者在參考、複製或修改本專案時，必須自行承擔全部風險和責任。** 如果您不具備相關的專業電氣知識和技能，請勿嘗試製作或使用本設備。

---

## 功能特點 (Features)

*   **標準相容**: 遵循TES-0D-02-01標準的CAN Bus通訊協議。
*   **通用性設計**: 支援最高DC 120V輸出，適用於多種車輛。
*   **可調輸出(可選)**: 可調最大電壓、電流、SOC。
*   **通訊抗干擾**: 包含了針對底層通訊不穩定的軟體補償策略（數據擾動）。
*   **狀態顯示**: 透過LED燈及OLED螢幕顯示待機、充電中、錯誤等狀態。
*   **安全保護**: 包含基礎的狀態機安全檢查和緊急停止功能。

## 硬體需求 (Hardware Requirements)

詳細的硬體材料，請參考Release中的 **BOM.md** 文件。

## 軟體與函式庫依賴 (Software & Dependencies)

本專案基於Arduino框架開發。請在Arduino IDE的函式庫管理員中安裝以下函式庫：

*   **mcp_can**: https://github.com/coryjfowler/MCP_CAN_lib
*   **ModbusMaster**: https://github.com/4-20ma/ModbusMaster
*   **U8g2**: https://github.com/olikraus/u8g2 
## 安裝與使用 (Installation & Usage)

1.  **硬體連接**: 根據電路圖（如果有的話）或程式碼中的引腳定義連接所有硬體。
   
    *詳細的硬體接線方法，請參考專案中的 **Wiring_Guide.md** 文件。
    
2.  **函式庫安裝**: 確保已安裝所有必要的函式庫。
3.  **程式碼配置**: 在程式碼頂部根據您的硬體配置修改引腳定義和充電樁物理極限參數。
4.  **編譯與上傳**: 將程式碼上傳到您的ESP32開發板。
5.  **測試**: **務必在連接到實際車輛前，在低壓和受控環境下進行充分測試！**

*  本程式I2C預設會掃描 0x3C 和 0x3D 地址。如果您的OLED地址不同，請修改 findOledDevice() 函數中的地址列表。
*  設定選單在有安裝OLED模組時才會啟用
*  **CAN Bus模組要確認無短路才接上，否則會燒BMS!!!**
  
**操作方式**

在待機時，按下Setting按鈕一至兩秒鐘進入設定選單，此時按鈕轉變為短按觸發，Start 和 Stop 按鈕轉變為 上一項/增加 和 下一項/減少，Setting 按鈕轉變為 確認。

## 授權 (License)

本專案採用 **[創用CC 姓名標示-非商業性-相同方式分享 4.0 國際 (CC BY-NC-SA 4.0)](https://creativecommons.org/licenses/by-nc-sa/4.0/deed.zh_TW)** 授權條款。

您可以自由地分享與改作本專案，惟需遵守「姓名標示」、「非商業性使用」及「相同方式分享」等條款。詳情請參閱授權協議全文。

Copyright (c) 2025 黃丞左(Chris Huang)
