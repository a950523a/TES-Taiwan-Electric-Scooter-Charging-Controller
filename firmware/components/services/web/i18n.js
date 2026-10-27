// i18n.js — 網頁的中／英切換（2026-10-17）。served at /i18n.js；/control、/、/hw 都載入它。
//
// 頁面本身照舊用中文寫，這裡是一張「中文 → 英文」對照表。英文模式時：
//   * 把頁面上的文字節點、placeholder／title、document.title 換成英文
//   * 用 MutationObserver 持續處理之後才畫出來的內容（狀態每秒更新、展開的面板…）
//   * alert／confirm 的訊息也翻
// 比對順序：整段完全相同 → 含 {} 的樣式（{} 是數字或其他會變的部分）→ 在長句裡替換認得的片段。
// 新增中文字串時把翻譯加到 EN；漏掉的不會出錯，只是英文模式下那段還是中文。
// 語言存在 localStorage（tes_lang），沒存過就跟著瀏覽器語言。切換會重新載入頁面。
(function () {
  'use strict';
  var KEY = 'tes_lang';
  var lang;
  try { lang = localStorage.getItem(KEY); } catch (e) { lang = null; }
  if (lang !== 'zh' && lang !== 'en') {
    var nav = (navigator.language || 'zh').toLowerCase();
    lang = nav.indexOf('zh') === 0 ? 'zh' : 'en';
  }

  var EN = {
    // 記住的 WiFi、自動熱點（2026-10-17）
    '記住的 WiFi（最多 5 組，會連訊號最強的那組）': 'Saved WiFi networks (up to 5; the strongest one is used)',
    '還沒有記住的 WiFi': 'No saved WiFi networks yet',
    'WiFi 名稱': 'WiFi name',
    'WiFi 密碼（開放網路留空）': 'WiFi password (empty for an open network)',
    '＋ 加入這組 WiFi': '+ Add this WiFi',
    '● 連線中': '● connected',
    '移除': 'Remove',
    '連不上記住的 WiFi 時（開機 30 秒、斷線 60 秒後）會自動開熱點': 'When no saved WiFi can be reached (30 s after boot, 60 s after losing it) the charger opens its own hotspot',
    '，密碼': ', password',
    '（OLED 設定選單的「熱點密碼」也看得到）。手機連上後開': '(also shown in the OLED settings menu as "AP pass"). Join it with the phone, then open',
    '出門充電時先把當地的 WiFi 加進來，之後去那裡就會自動連上，推播與遠端監控照常。': 'When charging away from home, add the local WiFi once; next time it connects by itself and notifications and remote monitoring keep working.',
    '請輸入 WiFi 名稱': 'Enter the WiFi name',
    'WiFi 名稱太長：最多 32 bytes（中文約 10 個字）': 'The WiFi name is too long: at most 32 bytes (about 10 Chinese characters)',
    '密碼要 8–63 個字（開放網路請留空）': 'The password must be 8–63 characters (leave it empty for an open network)',
    '已經記住 5 組了，先移除一組': '5 networks are already saved; remove one first',
    '✓ 已更新「{}」': '✓ Updated "{}"',
    '✓ 已加入「{}」，不用重開機；在範圍內的話幾秒內就會去連': '✓ Added "{}". No restart needed; if it is in range the charger connects within seconds',
    '✗ 加入失敗': '✗ Could not add it',
    '目前就是連著「{}」，移除後會馬上斷線、改找其他記住的 WiFi（都沒有就開熱點）。確定移除？': 'The charger is connected to "{}" right now. Removing it disconnects immediately and it looks for another saved network (or opens its hotspot). Remove it?',
    '移除「{}」？': 'Remove "{}"?',
    '✓ 已移除「{}」': '✓ Removed "{}"',
    '✗ 移除失敗': '✗ Could not remove it',
    '充電中（PSU 走 ESP-NOW）不能掃描，充完再掃': 'Cannot scan while charging with an ESP-NOW PSU; scan after charging',
    '已連線 · 熱點開著': 'Connected · hotspot on',
    'AC 輸入／可用功率': 'AC input / available power',
    '電流過大': 'Over current',
    '電流量測到 {} A，超過最大電流設定 1.5 A 以上、持續 1 秒，已停止輸出（車輛當時要求 {} A）。': 'The measured current was {} A, more than 1.5 A above the Max Current setting for 1 s; output stopped (the vehicle was asking for {} A).',
    '旋鈕電源的電流開得太大：把電源的電流旋鈕調低到最大電流以下（接電流量測節點時最多 15 A），再重新開始充電。': 'The knob power supply is set too high: turn its current knob below Max Current (at most 15 A with the current meter node), then start charging again.',
    '電源最多 {} A。插座功率不夠時，充電中會依實際電壓自動再降（見狀態卡的「可用功率」）': 'The power supply gives at most {} A. When the outlet cannot supply enough power, charging lowers it further by the actual voltage (see "available power" on the status card)',
    'AC 還沒讀到，不限': 'AC not read yet, no limit',
    'AC 樁 {} A': 'AC station {} A',
    '插座 {} A': 'outlet {} A',
    '插座不限': 'outlet unlimited',
    '電流上限 {} A（比最大電流設定小）': 'Current limited to {} A (below the Max Current setting)',
    '新的熱點密碼（8–63 字）': 'New hotspot password (8–63 characters)',
    '修改熱點密碼': 'Change hotspot password',
    '熱點密碼要 8–63 個英文字母、數字或符號（不能有空白）': 'The hotspot password must be 8–63 letters, digits or symbols (no spaces)',
    '✓ 熱點密碼已改好。熱點正開著的話，下次開熱點才會用新密碼': '✓ Hotspot password changed. If the hotspot is on right now, the new password applies the next time it opens',
    '✗ 修改失敗': '✗ Could not change it',
    '未連線 · 熱點開著': 'Not connected · hotspot on',
    '目前的連線不受影響。記住的 WiFi 都會保留，開關打開即可連回。': 'The current connection is not affected. Saved networks are kept; turn this back on to reconnect.',
    '✓ 已儲存。重啟後將連線至記住的 WiFi。': '✓ Saved. After a restart it connects to the saved WiFi.',
    // ── 共用 ──
    '中文': '中文', '讀取中': 'Loading', '讀取中…': 'Loading…', '錯誤': 'Error', '未知': 'Unknown',
    '已連線': 'Connected', '未連線': 'Not connected', '離線': 'Offline', '正常': 'Normal', '無': 'None',
    '關閉': 'Off', '關': 'Off', '開': 'On', '亮': 'On', '滅': 'Off', '停用': 'Disabled', '故障': 'Fault',
    '緊急停止': 'Emergency stop', '充電中': 'Charging', '待機中': 'Standby', '待機': 'Standby',
    '連線中': 'Connecting', '連線中…': 'Connecting…', '預充電': 'Pre-charge', '結束中': 'Ending', '收尾中': 'Finalizing',
    '充電完成': 'Charge complete', '搜尋中…': 'Searching…', '發送中…': 'Sending…', '送出中…': 'Sending…',
    '儲存中…': 'Saving…', '掃描中…': 'Scanning…', '✓ 已複製': '✓ Copied', '✗ 複製失敗': '✗ Copy failed',
    '✗ 讀取失敗': '✗ Failed to load', '✗ 連線錯誤': '✗ Connection error', '✗ 連線錯誤，請重試': '✗ Connection error, please retry',
    '（': ' (', '）': ')', '。': '.', '，': ', ', '／': ' / ',

    // ── /control 頁首、狀態 ──
    'TES充電': 'TES Charger', '裝置列表': 'Devices', '輸出電壓': 'Output voltage', '輸出電流': 'Output current',
    '即時功率': 'Power', '累積電量': 'Energy', '剩餘時間': 'Time left', '▶ 開始充電': '▶ Start charging',
    '◼ 停止充電': '◼ Stop charging', 'WiFi 狀態': 'WiFi', 'IP 位址': 'IP address', '主機名稱': 'Hostname',
    'PSU 電源': 'PSU', '📶 切換手機 WiFi': '📶 Switch phone WiFi', '充電電壓 / 停止電壓': 'Voltage / stop voltage',
    '(預估)': '(estimated)', '已充入 / 目標': 'Delivered / target', '已充時間': 'Charged for',
    '⚠ 電源電壓 {} V，超過這塊板的上限 {} V。': '⚠ PSU voltage {} V is above this board\'s limit of {} V.',
    '這塊板量不準更高的電壓，最大電壓只會用到上限——請把電源調低到上限以下。':
      'This board cannot measure higher voltages and caps Max Voltage at the limit — turn the PSU down below it.',
    'ESP-NOW 失連': 'ESP-NOW lost', 'ESP-NOW 需重新配對': 'ESP-NOW needs re-pairing', 'ESP-NOW 未配對': 'ESP-NOW not paired',
    '目前版本：': 'Firmware: ', '硬體：': 'Hardware: ',
    '無法辨識（ID': 'Unrecognised (ID ',
    'V，已用舊板分壓係數，電壓讀值不可信，請更新韌體）': ' V; using the old divider, voltage readings are not reliable — update the firmware)',
    '（依出廠紀錄；AIN3 硬體 ID 讀到': ' (from the factory record; AIN3 hardware ID reads ',
    'V，與出廠紀錄不符，請檢查 R36／R37）': ' V, which does not match — check R36/R37)',

    // ── 韌體更新、重新啟動 ──
    '🔄 韌體更新': '🔄 Firmware update', '進度': 'Progress', '⬆ 更新至最新韌體': '⬆ Update to latest firmware',
    '手動上傳韌體 (.bin)': 'Upload firmware (.bin)', '📤 上傳韌體': '📤 Upload firmware', '🔁 重新啟動控制器': '🔁 Restart controller',
    '🔧 硬體狀態（腳位、ADC、PSU、CAN、系統）→': '🔧 Hardware status (pins, ADC, PSU, CAN, system) →',
    '作者：Chris Huang 授權：CC BY-NC-SA 4.0': 'Author: Chris Huang · Licence: CC BY-NC-SA 4.0',
    '更新中，請勿斷電…': 'Updating, do not power off…', '✓ 更新完成，重新啟動中…': '✓ Update done, restarting…',
    '確定要更新韌體嗎？更新期間請勿斷電。': 'Update the firmware? Do not power off during the update.',
    '✗ 無法啟動更新': '✗ Could not start the update',
    '確定要重新啟動控制器嗎？': 'Restart the controller?',
    '充電中無法重新啟動；設定選單中未儲存的變更會遺失。': 'It cannot restart while charging; unsaved changes in the settings menu will be lost.',
    '✗ 充電中無法重新啟動，請先停止充電': '✗ Cannot restart while charging — stop charging first',
    '✗ 無法重新啟動': '✗ Could not restart', '重新啟動中，請稍候…': 'Restarting, please wait…',
    '✗ 60 秒內沒有回應，請確認控制器有電、WiFi 正常後重新整理頁面': '✗ No response within 60 s — check the controller\'s power and WiFi, then reload',
    '請先選擇 .bin 檔案': 'Choose a .bin file first', '上傳中，請勿斷電…': 'Uploading, do not power off…',
    '✓ 上傳完成，重新啟動中…': '✓ Upload done, restarting…', '✗ 上傳失敗': '✗ Upload failed',

    // ── 設定 ──
    '⚙ 設定': '⚙ Settings', '裝置名稱': 'Device name', '例如：車庫充電樁': 'e.g. Garage charger',
    '同一個 WiFi 下有多台時用來分辨。網址': 'Tells units apart on the same WiFi. The address',
    '由 MAC 產生、改名也不會變。': 'comes from the MAC and does not change when renamed.',
    '自動設定電壓上限（依開機 ADC 量測）': 'Set the voltage limit automatically (measured at boot)',
    '最大電壓 (V)': 'Max voltage (V)', '電壓安全上限 (V)': 'Voltage safety limit (V)', '最大電流 (A)': 'Max current (A)',
    '充電停止條件': 'Stop charging by', '依 SOC 停止': 'SOC', '依電壓停止': 'Voltage', '依時間停止': 'Time',
    '依充入電量停止': 'Energy delivered', '目標 SOC (%)': 'Target SOC (%)', '充電停止電壓 (V)': 'Stop voltage (V)',
    '充電時長上限（分鐘，10–600）': 'Charge time limit (min, 10–600)', '充入電量目標（kWh，0.1–20）': 'Energy target (kWh, 0.1–20)',
    '目前沒有接可程式電源，充入電量是用「最大電流」設定值估算的。電池快滿時 BMS 會降電流、估算值偏高，可能比目標':
      'No programmable PSU is connected, so the energy is estimated from the Max Current setting. Near full the BMS lowers the current and the estimate reads high, so charging may',
    '提早停止': ' stop early', '（不會多充）。': ' (never late).',
    '月充電量換算：電源效率（%，50–100）': 'Monthly energy: PSU efficiency (%, 50–100)',
    '每度電價（元，0 = 不換算電費）': 'Price per kWh (NT$, 0 = no cost estimate)',
    '連線至 WiFi 路由器': 'Connect to a WiFi router', '⚠️ 目前設定為': '⚠️ Currently set to ',
    '不連線至路由器': 'not connect to the router', '。下次重新開機後將固定開啟熱點': '. After the next restart it will always run its own hotspot ',
    '，這個網址會失效 —— 需要把手機 WiFi 改連上述熱點，再開啟': '; this address will stop working — connect the phone to that hotspot and open ',
    '才回得來。': ' to get back.',
    '目前的連線不受影響。SSID 與密碼已保留，開關打開即可連回。': 'The current connection is not affected. The SSID and password are kept; turn this on to reconnect.',
    '路由器名稱': 'Router name (SSID)', '🔍 掃描': '🔍 Scan', 'WiFi 密碼（留空保持不變）': 'WiFi password (leave empty to keep)',
    'LuxBeacon SOC 燈號': 'LuxBeacon SOC light', '🔔 推播通知': '🔔 Notifications', 'ntfy.sh（推薦）': 'ntfy.sh (recommended)',
    '自定義 Webhook': 'Custom webhook', '推播頻道網址': 'Channel URL', '📋 複製': '📋 Copy', '🎲 重新產生': '🎲 New',
    '📲 訂閱': '📲 Subscribe', '📨 測試': '📨 Test', '① 下載 ntfy app：': '① Get the ntfy app: ',
    '② 點「訂閱」自動帶入頻道，或在 app 手動輸入上方網址': '② Tap "Subscribe" to add the channel, or enter the URL above in the app',
    'https://ntfy.sh/your-topic 或任意 Webhook URL': 'https://ntfy.sh/your-topic or any webhook URL',
    '📡 MQTT 遠端監控': '📡 MQTT remote monitoring', 'Broker URL（空白 = 停用）': 'Broker URL (empty = disabled)',
    'mqtts://帳號:密碼@xxxx.emqxsl.com:8883': 'mqtts://user:pass@xxxx.emqxsl.com:8883',
    '允許透過 MQTT 遠端啟停充電': 'Allow remote start/stop over MQTT', '⚠️ 開啟後，': '⚠️ When on, ',
    '任何能對這個 broker 發佈訊息的人': 'anyone who can publish to this broker',
    '都能啟動或停止你的充電器。': ' can start or stop your charger.', '在': 'On ', '免帳號的公開 broker': 'a public broker without accounts',
    '上這特別危險 —— 對方可以用萬用字元 （': ' this is especially risky — a wildcard (',
    '）直接列出所有在線裝置，不需要猜你的 topic。': ') lists every online unit without guessing your topic.',
    '建議：改用需要帳號密碼的 broker、URL 用': 'Use a broker with a username and password, ',
    '加密， 或關掉這個開關（狀態仍會發佈，只是不接受指令）。': ' for encryption, or turn this off (status is still published, commands are ignored).',
    'URL 可直接帶帳密與 TLS：': 'The URL can carry credentials and TLS: ', 'mqtts://帳號:密碼@broker:8883': 'mqtts://user:pass@broker:8883',
    '免費、需帳密的雲端 broker：EMQX Cloud Serverless（HiveMQ Cloud 的免費方案已停止新申請）。 手機 App 的「遠端監控」精靈會幫你填好這裡。':
      'Free cloud broker with accounts: EMQX Cloud Serverless (HiveMQ Cloud no longer takes new free sign-ups). The app\'s Remote wizard fills this in for you.',
    '公開 broker（如': 'A public broker (e.g. ', '）任何人都能讀取你的充電狀態， 僅建議測試用。': ') lets anyone read your charging status — for testing only.',
    'MQTT 設定變更後需重啟生效': 'MQTT changes take effect after a restart', '⏰ 定時充電': '⏰ Scheduled charging',
    '啟用定時充電': 'Enable schedule', '開始充電時間': 'Start time', '結束充電時間': 'End time', '到時自動停止充電': 'Stop automatically at the end time',
    'NTP：讀取中…': 'NTP: loading…', '時區 UTC+8 ∙ 隔夜窗口（如 23:00–06:00）自動支援': 'Time zone UTC+8 ∙ overnight windows (e.g. 23:00–06:00) work',
    'NTP：已同步（': 'NTP: synced (', 'NTP：等待同步…（WiFi 連線後自動同步）': 'NTP: waiting… (syncs once WiFi is up)',
    '🔌 PSU 電源設定': '🔌 PSU', 'UART（有線）': 'UART (wired)', 'ESP-NOW（無線）': 'ESP-NOW (wireless)',
    '配對狀態：讀取中…': 'Pairing: loading…', '📡 開始配對': '📡 Pair',
    '按下後在 PSU 端也開始配對。兩邊會顯示同一組 6 位數配對碼， 數字相同才在兩邊按確認 —— 跟藍牙配對一樣。配對後每則無線訊息都會驗證，別人無法冒充。':
      'Then start pairing on the PSU too. Both show the same 6-digit code; confirm on both only if they match — like Bluetooth. After pairing every wireless message is authenticated, so nobody can impersonate either side.',
    'Transport 變更後需儲存設定並重啟生效': 'Changing the transport needs Save and a restart',
    '配對狀態：舊版配對（沒有驗證金鑰）—— 請重新配對，PSU 韌體也要更新': 'Pairing: old pairing without a key — pair again (update the PSU firmware too)',
    '配對狀態：已配對 MAC': 'Pairing: paired, MAC', '(未知)': '(unknown)', '配對狀態：未配對': 'Pairing: not paired',
    '⚡ 自動充電': '⚡ Auto start', '插槍或 CAN 信號自動啟動充電': 'Start charging when plugged in or on a CAN permit',
    '⚠️ 最大電流超過 15A，自動充電已停用。請將最大電流降至 15A 以內才可使用此功能。':
      '⚠️ Max current is above 15 A, so auto start is disabled. Set it to 15 A or less to use this.',
    '啟用後 VP 繼電器常通，偵測到 CP 插入或車端 CAN 許可時自動開始充電。':
      'When on, VP stays powered and charging starts when CP appears or the vehicle grants permission over CAN.',
    '一般故障 10 秒後自動復歸；按下緊急停止鈕後仍需手動 Reset Fault；充電完成後須斷槍再重插才能再次觸發。':
      'Most faults clear after 10 s; after the emergency button a manual Reset Fault is needed; after a full charge, unplug and plug in again to trigger it again.',
    '🖥 螢幕（OLED）': '🖥 Display (OLED)', '顯示語言': 'Language', '充電中動畫': 'Charging animation',
    '電池＋箭頭': 'Battery + arrows', '只要電池': 'Battery only', '只要箭頭': 'Arrows only',
    '螢幕保護（待機多久沒人碰）': 'Screensaver (idle time in standby)', '5 分鐘': '5 min', '15 分鐘': '15 min', '30 分鐘': '30 min',
    '螢幕保護樣式': 'Screensaver style', '游泳的烏龜': 'Swimming turtle', '全黑': 'Dark',
    '待機時畫面每分鐘會偏移 1 像素防烙印（一律開著）。螢幕保護會顯示狀態、時間、電源電壓，每分鐘換位置； 喚醒的那一下按鍵只會喚醒，不會開始或停止充電。故障一發生會立刻蓋過動畫與螢幕保護。 改完按下方的「儲存」。':
      'In standby the screen shifts 1 px every minute against burn-in (always on). The screensaver shows state, time and PSU voltage, moving every minute; the key press that wakes it does nothing else. A fault immediately replaces any animation or screensaver. Press Save below when done.',
    '項設定未儲存': ' unsaved changes', '捨棄': 'Discard', '💾 儲存': '💾 Save',
    '關閉後，控制器下次重新開機將不再連上路由器，改為固定開啟熱點': 'When off, after the next restart the controller will not join the router and will run its own hotspot ',
    '目前的連線不會馬上中斷，但重開機後這個網址會失效 —— 需要改連上述熱點、': 'The current connection stays up, but after a restart this address stops working — join that hotspot and ',
    '開啟 192.168.4.1 才回得來。': 'open 192.168.4.1 to get back.',
    'SSID 與密碼會保留，開關打開即可連回。': 'The SSID and password are kept; turn this on to reconnect.',
    '確定要關閉嗎？': 'Turn it off?',
    '開啟後，任何能對這個 MQTT broker 發佈訊息的人都能啟動或停止你的充電器。': 'When on, anyone who can publish to this MQTT broker can start or stop your charger.',
    '若你用的是免帳號的公開 broker，對方可以用萬用字元列出所有在線裝置，': 'On a public broker without accounts, a wildcard lists every online unit ',
    '不需要知道你的 topic。': 'without knowing your topic.', '建議改用需要帳密的 broker 並使用 mqtts://。': 'Use a broker with a username and password, and mqtts://.',
    '確定要開啟嗎？': 'Turn it on?',
    '啟用後 VP 繼電器將常時通電，偵測到插槍或車端 CAN 許可就會自動開始充電，': 'When on, VP stays powered and charging starts on plug-in or a CAN permit ',
    '不需要有人按下開始。': 'without anyone pressing Start.', '這是 Beta 功能，尚未經過實車驗證。': 'This is a beta feature, not yet tested on a vehicle.',
    '確定要啟用嗎？': 'Enable it?',
    '✓ 已開啟 WiFi 連線 — 重啟後生效': '✓ Router connection on — applies after a restart',
    '✓ 已關閉 WiFi 連線 — 重啟後改用熱點': '✓ Router connection off — hotspot after a restart',
    '✓ 已儲存': '✓ Saved', '✗ 儲存失敗，開關已還原': '✗ Save failed, switch restored', '✗ 連線錯誤，開關已還原': '✗ Connection error, switch restored',
    '✗ 儲存失敗': '✗ Save failed', '✗ 儲存失敗，請重試': '✗ Save failed, please retry',
    '✓ 已儲存。設備將重啟連接至': '✓ Saved. The unit will restart and join ', '請將手機 WiFi 切換至相同網路，再開啟': 'Switch the phone to the same network, then open ',
    '✓ 已儲存 — 見下方重新連線說明': '✓ Saved — see how to reconnect below', '✓ 已儲存。重啟後將連線至': '✓ Saved. After a restart it will join ',
    '已儲存的 WiFi': 'the saved WiFi', '✓ 已儲存。重啟後將': '✓ Saved. After a restart it will ', '固定開啟熱點': 'always run its hotspot',
    '，請改連': '; join ', '，再開啟': ', then open ', '✓ 已儲存 — 見下方連線方式': '✓ Saved — see how to connect below',
    '✓ 已儲存。PSU 傳輸模式變更後需手動重啟設備生效。': '✓ Saved. Restart the unit for the PSU transport change to apply.',
    '✓ 已儲存 — 需重啟生效': '✓ Saved — restart to apply', '✓ 測試推播已發送': '✓ Test notification sent', '發送失敗': 'Send failed',
    '未找到網路': 'No networks found', '✗ 掃描失敗': '✗ Scan failed',

    // ── PSU 配對視窗 ──
    '📡 PSU 配對': '📡 PSU pairing', '數字相同，確認': 'Codes match — confirm', '不同／取消': 'Different / cancel',
    '配對失敗': 'Pairing failed', '逾時 —— 沒找到 PSU，或太久沒有確認': 'Timed out — no PSU found, or not confirmed in time',
    '已在這裡取消': 'Cancelled here', 'PSU 端取消了': 'Cancelled on the PSU',
    '安全檢查失敗（承諾值不符）—— 可能有人在中間攔截，請遠離其他裝置再試': 'Security check failed (commitment mismatch) — someone may be intercepting; retry away from other devices',
    '安全檢查失敗（金鑰確認不符）—— 可能有人在中間攔截，請遠離其他裝置再試': 'Security check failed (key confirmation mismatch) — someone may be intercepting; retry away from other devices',
    '加密運算錯誤': 'Crypto error', '已確認，等 PSU 端按確認…': 'Confirmed — waiting for the PSU…',
    'PSU 螢幕上的數字': 'Is the number on the PSU screen ', '完全相同': 'exactly the same',
    '嗎？相同才確認；不同代表配到別台或有人攔截，請取消。': '? Confirm only if it is; a different number means another unit or an interception — cancel.',
    '控制器上按 START＝確認、STOP＝取消，效果一樣。': 'On the controller, START = confirm and STOP = cancel work too.',
    '找到 PSU，交換金鑰中…': 'PSU found, exchanging keys…', '搜尋中…請在 PSU 端開始配對。': 'Searching… start pairing on the PSU.',
    '⚠ 收到舊版 PSU 韌體的配對訊號 —— 它不支援新的安全配對，請先更新 PSU 韌體。': '⚠ Pairing request from old PSU firmware — it does not support secure pairing; update the PSU first.',
    '✅ 配對完成': '✅ Paired', '之後每則無線訊息都會驗證，其他裝置無法冒充這台控制器或 PSU。': 'Every wireless message is now authenticated; no other device can impersonate this controller or the PSU.',
    '✗ 配對失敗': '✗ Pairing failed', '。沒有儲存任何東西。': '. Nothing was saved.',

    // ── 遠端監控卡、CAN 診斷 ──
    '📡 遠端監控': '📡 Remote monitoring', '透過 MQTT 在任何網路環境即時監控充電狀態並遠端 Start / Stop': 'Watch charging from any network over MQTT, with remote Start / Stop',
    'Cloud PWA 連結': 'Cloud PWA link', '📋 複製連結': '📋 Copy link', '🔗 開啟監控': '🔗 Open monitor', '在手機上開啟後可加入主畫面作為獨立 App': 'Open it on a phone and add it to the home screen as an app',
    '🔌 CAN 診斷': '🔌 CAN diagnostics', '▶ 展開': '▶ Expand', '▼ 收起': '▼ Collapse', '最後收到／發出的 CAN 解碼值，未接車時全為 0': 'Last CAN frames received and sent, decoded; all 0 without a vehicle',
    '匯流排狀態': 'Bus', '控制器狀態': 'Controller state', '錯誤計數 TX/RX': 'Error counters TX/RX', '匯流排錯誤': 'Bus errors',
    '仲裁失敗 / 漏收': 'Arbitration lost / missed', '送出失敗': 'TX failed', '車輛狀態': 'Vehicle status', '充電許可 (bit0)': 'Charge permit (bit0)',
    '接觸器 (bit1)': 'Contactor (bit1)', '充電姿勢 (bit2)': 'Charging posture (bit2)', '正常停止要求 (bit3)': 'Normal stop request (bit3)',
    '故障旗標': 'Fault flags', '請求電流': 'Requested current', '充電電壓上限': 'Charge voltage limit', '最大允許電壓': 'Max allowed voltage',
    '車輛參數': 'Vehicle parameters', '序列號': 'Sequence', '最大充電時間': 'Max charge time', '預估結束時間': 'Estimated end',
    '車輛緊急': 'Vehicle emergency', '緊急旗標': 'Emergency flags', '緊急停止請求 (bit0)': 'Emergency stop request (bit0)',
    '熔接異常 (bit1)': 'Welding fault (bit1)', '本次最大充電電流': 'Max current this session', '車輛製造商編號': 'Vehicle maker ID',
    '充電樁狀態': 'Charger status', '停止控制 (bit0)': 'Stop control (bit0)', '裝置狀態 (bit1)': 'Device state (bit1)', '電子鎖 (bit2)': 'Lock (bit2)',
    '可提供電壓 (VLIM)': 'Available voltage (VLIM)', '可提供電流 (I_MX)': 'Available current (I_MX)', '異常判定電壓上限 (VLIM2)': 'Fault detection voltage (VLIM2)',
    '充電樁參數': 'Charger parameters', '額定功率': 'Rated power', '實際輸出電壓': 'Actual voltage', '實際輸出電流': 'Actual current',
    '充電樁緊急': 'Charger emergency', '緊急停止 (bit0)': 'Emergency stop (bit0)',
    '已停止': 'Stopped', '運作中': 'Running', '復歸中': 'Recovering', '未收到': 'Not received', '{}s 前 · {} 幀': '{} s ago · {} frames',
    '已送 {} 幀': '{} frames sent', '許可': 'Permitted', '未許可': 'Not permitted', '開路': 'Open', '閉合': 'Closed', '不可充電': 'Not OK to charge',
    '可充電': 'OK to charge', '正常停止': 'Normal stop', '0x00 正常': '0x00 normal', '緊急停止請求': 'Emergency stop request',
    '熔接異常': 'Welding fault', '停止控制中': 'Stopping', '輸出運轉中': 'Output running', '閉鎖中': 'Locked', '解除': 'Unlocked',

    // ── 故障說明 ──
    '供電系統異常': 'Supply system fault', '電池過電壓': 'Battery over-voltage', '電池電壓不足': 'Battery under-voltage',
    '電池電流差異異常': 'Battery current mismatch', '電池高溫異常': 'Battery over-temperature', '電池電壓差異常': 'Battery voltage mismatch',
    '車輛允許充電': 'Vehicle permits charging', '接觸器斷開／熔接診斷結束': 'Contactor open / weld check done', '車輛姿勢不可充電': 'Vehicle posture not OK to charge',
    '車輛要求正常停止': 'Vehicle requests a normal stop', '直流供電裝置異常': 'DC supply device fault', '電池不適合': 'Battery not suitable',
    '車輛沒有回應': 'No response from the vehicle',
    '等了 {} 秒仍未收到車輛的充電許可（CAN 0x500 bit0）。': 'No charge permit from the vehicle after {} s (CAN 0x500 bit0).',
    '最後收到的車端狀態是 0x{}。': 'Last vehicle status: 0x{}.',
    '確認充電槍已插到底、CAN 線（H/L）沒有鬆脫或接反，並確認車輛已進入充電模式。': 'Check the connector is fully in, the CAN wires (H/L) are tight and not swapped, and the vehicle is in charging mode.',
    '電源電壓不夠高': 'PSU voltage too low', '車輛要求': 'The vehicle asks for ', '，但這台控制器能提供的只有': ', but this controller can only supply ',
    '（最大電壓設定，或自動偵測到的電源電壓）。': ' (Max Voltage setting, or the measured PSU voltage).',
    '請換用輸出電壓': 'Use a PSU with an output of ', '以上的電源。最大電壓要照電源實際能輸出的電壓設定，': ' or more. Set Max Voltage to what the PSU really outputs; ',
    '不要為了讓車開始充電而調高——它是車輛判斷過電壓的門檻。': 'do not raise it to get the vehicle to start — it is the vehicle\'s over-voltage threshold.',
    '預充電階段逾時': 'Pre-charge timed out', '等了 20 秒，CP 訊號與車端許可沒有同時就緒。': 'After 20 s, CP and the vehicle permit were not both ready.',
    '當下 CP = {} V，車端狀態 0x{}。': 'CP = {} V, vehicle status 0x{}.',
    '把充電槍拔起來重新插到底並鎖固；若 CP 電壓一直是 0 V，檢查 CP 線路。': 'Unplug, plug the connector fully in and lock it; if CP stays at 0 V, check the CP wiring.',
    '車輛的接觸器沒有閉合': 'Vehicle contactor did not close', '等了 {} 秒，車輛仍回報接觸器開路（狀態 0x{}）。': 'After {} s the vehicle still reports its contactor open (status 0x{}).',
    '這是車輛端的狀況。確認電池溫度、SOC 是否在可充電範圍，或查看車輛儀表板的訊息。': 'This is on the vehicle side. Check battery temperature and SOC are in range, or the dashboard messages.',
    '電源供應器斷線': 'PSU disconnected', '充電進行 {} 秒後與 PSU 失去通訊，已立即停止輸出。': 'Lost contact with the PSU after {} s of charging; output stopped at once.',
    '檢查 PSU 電源是否正常；UART 模式檢查接線，ESP-NOW 模式檢查距離與配對狀態。': 'Check the PSU has power; in UART mode check the wiring, in ESP-NOW mode the distance and pairing.',
    '車輛判定供電異常': 'Vehicle reports a supply fault', '車輛電池異常': 'Vehicle battery fault', '車輛偵測到：': 'The vehicle detected: ',
    '車輛送出故障旗標 {}（未定義的位元）。': 'The vehicle sent fault flags {} (undefined bits).',
    '注意：「供電系統異常」是車輛在指控': 'Note: "supply system fault" means the vehicle blames ', '充電樁這一端': 'the charger',
    '不是它自己的電池有問題。': 'not its own battery.',
    '先確認 0x508 送出的 VLIM2（故障偵測電壓）不是 0 —— 車輛會把 VLIM2=0 解讀成「輸出電壓 ≥ 0V 即異常」而立刻報錯。':
      'First check VLIM2 (fault detection voltage) in 0x508 is not 0 — the vehicle reads VLIM2 = 0 as "any output voltage is a fault".',
    '展開下方充電紀錄的 Log 對照發生時間點。': 'Expand the log in the charge history below to see when it happened.',
    '故障來自車輛電池端，請查看車輛儀表板的錯誤訊息，確認電池溫度與電量狀態。': 'The fault comes from the vehicle battery; check the dashboard, battery temperature and charge level.',
    '充電槍鬆脫': 'Connector came loose', '充電途中 CP 訊號中斷（掉到 {} V），已立即停止輸出。': 'CP dropped during charging (to {} V); output stopped at once.',
    '把充電槍重新插到底並確認卡榫鎖固。': 'Plug the connector fully back in and make sure the latch locks.',
    '緊急停止鈕被按下': 'Emergency button pressed', '有人按下了緊急停止按鈕，輸出已切斷。': 'Someone pressed the emergency stop; output is cut.',
    '排除現場狀況後，長按 SETTING 進入選單執行': 'Once it is safe, long-press SETTING and run ', '才會解除（此項不會自動復歸）。': ' to clear it (it does not clear by itself).',
    '車輛要求緊急停止': 'Vehicle requested an emergency stop', '車輛送出了 0x5F0 緊急停止訊框。': 'The vehicle sent a 0x5F0 emergency stop frame.',
    '注意：eMoving iE125 在每次': 'Note: the eMoving iE125 sends this ', '正常充電結束後': 'after every normal charge end',
    '都會送這個訊框，多半不是故障。': ', so it is usually not a fault.',
    '若充電已完成則無須處理，5 秒後會自動復歸。若是充電中途出現，請查看車輛儀表板。': 'If charging had finished, nothing to do — it clears in 5 s. If it happened mid-charge, check the dashboard.',
    '未知故障（代碼 {}）': 'Unknown fault (code {})',

    // ── 充電紀錄 ──
    '📋 充電紀錄': '📋 Charge history', '↻ 重新整理': '↻ Refresh', '日期': 'Date', '充電時間': 'Duration', '電量 (Wh)': 'Energy (Wh)', '線上更新中，請勿斷電…': 'Updating online — do not power off…', '上傳更新中，請勿斷電…': 'Uploading the update — do not power off…', '✓ 更新完成，重新啟動中…': '✓ Update done, restarting…', '✗ 更新失敗：{}（原本的韌體照常運作）': '✗ Update failed: {} (the old firmware keeps running)', '✗ 更新失敗（原本的韌體照常運作）': '✗ Update failed (the old firmware keeps running)', '✗ 無法開始更新：{}': '✗ Could not start the update: {}', '✗ 無法開始更新': '✗ Could not start the update', '✗ 韌體更新中，更新完成前不能開始充電': '✗ A firmware update is running; charging cannot start until it finishes', '✗ 90 秒內沒有回應，請確認控制器有電、WiFi 正常後重新整理頁面': '✗ No answer within 90 s — check that the controller has power and WiFi, then reload', '連線中斷': 'connection lost', '充入': 'Charged', '停止原因': 'Stop reason',
    '有 ▸ 的列可以展開，看該次的充電曲線與詳細 Log': 'Rows with ▸ expand to show that session\'s curve and detailed log',
    '曲線與詳細 Log 存放在記憶體，': 'Curves and logs are kept in RAM and ', '控制器一斷電就會清除': 'are lost when the controller powers off',
    '， 因此重開機之前的紀錄只剩上表的摘要（時長、電量、SOC、停止原因），沒有 ▸ 可以展開。 摘要本身存在 NVS，不會消失。':
      ', so sessions before the last restart only have the summary above (duration, energy, SOC, stop reason) and no ▸. The summary is in NVS and is kept.',
    'SOC 達標': 'SOC reached', '手動停止': 'Stopped by user', 'BMS 停止': 'Stopped by BMS', '計時完成': 'Timer done', '電壓達標': 'Voltage reached', '電量達標': 'Energy reached',
    '此次沒有曲線資料': 'No curve data for this session', '電壓': 'Voltage', '電流': 'Current', '（BMS 要求 {} A）·': ' (BMS asks {} A) ·',
    '📈 充電曲線': '📈 Charge curve', '📝 詳細 Log': '📝 Detailed log', '⬇ 下載': '⬇ Download', '過濾關鍵字…': 'Filter…',
    '本月充入': 'This month', '（{} 次）': ' ({} sessions)', '（{} Ah，{} 次）': ' ({} Ah, {} sessions)', '約 {} 度 · 約 NT${}': '≈ {} kWh from the grid · ≈ NT${}',
    '{}：{} kWh，{} 次': '{}: {} kWh, {} sessions', '{}：{} kWh，{} Ah，{} 次': '{}: {} kWh, {} Ah, {} sessions', '（估算 {} kWh）': ' ({} kWh estimated)', '，約 NT${}': ', ≈ NT${}',
    '{}月': function (m) { return ['Jan', 'Feb', 'Mar', 'Apr', 'May', 'Jun', 'Jul', 'Aug', 'Sep', 'Oct', 'Nov', 'Dec'][+m - 1] || m; },
    '淡色為估算：沒接可程式電源時，電流以「最大電流」設定值計算': 'Lighter = estimated: without a programmable PSU the current is taken from Max Current',
    '{} 次充電開始時控制器還沒對到網路時間，未計入': '{} sessions started before the clock was set and are not counted',
    '電費以電源效率 {}%、每度 {} 元換算，僅供參考': 'Cost at {}% PSU efficiency and NT${} per kWh — an estimate only',
    '這是充進電池的電量；在設定填入每度電價，可換算成大約的電費': 'Energy into the battery; enter a price per kWh in Settings for an approximate cost',
    '開機 +{}': 'Boot +{}', '未完成': 'Incomplete', '尚無紀錄': 'No sessions yet',
    '此紀錄的曲線與詳細 Log 已隨控制器斷電清除，僅存摘要': 'This session\'s curve and log were lost at power-off; only the summary remains',

    // ── devices.html ──
    'TES 充電控制器': 'TES Charge Controller', '⚡ TES 充電控制器': '⚡ TES Charge Controller', '搜尋區域網路上的裝置…': 'Searching the local network…',
    '↻ 重新搜尋': '↻ Search again', '本機控制介面 →': 'This unit\'s controls →',
    '裝置清單透過 mDNS 搜尋取得，只會列出同一個網段的 TES 控制器。': 'Found with mDNS; only TES controllers on the same subnet are listed.',
    '若某台沒出現，可直接輸入它的': 'If one is missing, enter its ', '或 IP。': ' or IP directly.', '找不到任何裝置。': 'No devices found.',
    '若控制器剛開機，請等幾秒後再重新搜尋。': 'If a controller just booted, wait a few seconds and search again.', '位址未知': 'Address unknown',
    '只找到本機，進入控制介面…': 'Only this unit found, opening its controls…', '找到 {} 台裝置': '{} devices found',
    '✗ 搜尋失敗': '✗ Search failed', '無法取得裝置清單': 'Could not get the device list',

    // ── hw.html ──
    '硬體狀態 — TES': 'Hardware status — TES', '🔧 硬體狀態': '🔧 Hardware status', '← 控制介面': '← Controls', '點電路板上的零件看說明。': 'Tap a part on the board for details.',
    '依電路板的實際位置繪製。虛線框＝裝在背面。 亮綠＝輸出導通，藍＝按鈕按下，紅框＝異常。': 'Drawn at the real positions. Dashed = mounted on the back. Green = output on, blue = button pressed, red outline = fault.',
    '工作台測試模式': 'Bench test mode', 'DC 繼電器': 'DC relay', '電子鎖': 'Lock', '橘燈': 'Amber LED', '綠燈': 'Green LED', '紅燈': 'Red LED',
    '進入測試模式': 'Enter test mode', '輸出（腳位實際電位）': 'Outputs (actual pin levels)', '按鈕': 'Buttons', '類比量測（ADS1115）': 'Analog (ADS1115)',
    'PSU 連線': 'PSU link', 'CAN 控制器': 'CAN controller', '系統': 'System', '任務堆疊餘裕（歷史最低）': 'Task stack headroom (lowest)', 'TCP 連線（lwIP）': 'TCP connections (lwIP)',
    '查「/control 載入一兩次後變慢」用：崩的時候看大傳輸那條的重傳次數與逾時是否一直漲（送不出去）， 還是 TIME_WAIT 堆滿、新連線開不起來（TCP 資源）。':
      'For the "/control slows down after a load or two" issue: during a collapse, see whether the bulk transfer\'s retransmits and timeout keep rising (cannot send) or TIME_WAIT fills up so new connections fail (TCP resources).',
    '從未': 'Never', 'ms 前': ' ms ago', 's 前': ' s ago', '天': 'd ', '時': 'h ', '分': 'm ', '秒': 's',
    // 開機時間：hw.html 組成「1天 2時 3分 4秒」
    '{}天 {}時 {}分 {}秒': '{}d {}h {}m {}s', '{}時 {}分 {}秒': '{}h {}m {}s', '{}分 {}秒': '{}m {}s',
    '硬體狀態 — {}': 'Hardware status — {}',
    'START 按鈕': 'START button', 'STOP 按鈕': 'STOP button', 'SETTING 按鈕': 'SETTING button', '緊急停止按鈕': 'Emergency button',
    'IO40 — 按下即鎖存，需在選單 Reset Fault': 'IO40 — latches when pressed; clear with Reset Fault in the menu', 'BOOT 按鈕': 'BOOT button',
    'IO0 — 按住上電進入燒錄模式': 'IO0 — hold at power-on for flashing mode', 'EN 重置按鈕': 'EN reset button', '按下 ESP32 重開機': 'Restarts the ESP32',
    '橘燈（待機）': 'Amber LED (standby)', '綠燈（充電）': 'Green LED (charging)', '紅燈（故障）': 'Red LED (fault)',
    'DC 主繼電器驅動 MOSFET': 'DC relay driver MOSFET', 'DC 主繼電器線圈接頭': 'DC relay coil connector', 'DC 繼電器飛輪二極體': 'DC relay flyback diode',
    '電子鎖驅動 MOSFET': 'Lock driver MOSFET', '電子鎖接頭': 'Lock connector', '電子鎖飛輪二極體': 'Lock flyback diode', 'VP 驅動（N-MOS）': 'VP driver (N-MOS)',
    'VP 開關（P-MOS）': 'VP switch (P-MOS)', 'VP 輸出接頭': 'VP output connector', '送到車端的 12V': '12 V to the vehicle', 'CAN / CP 接頭': 'CAN / CP connector',
    'CANH、CANL、CP': 'CANH, CANL, CP', 'CAN 終端電阻': 'CAN termination', 'CAN 收發器': 'CAN transceiver', 'CP 分壓上臂': 'CP divider (top)', 'CP 分壓下臂': 'CP divider (bottom)',
    'AIN0-1 輸出電壓、AIN2 CP、AIN3 硬體 ID': 'AIN0-1 output voltage, AIN2 CP, AIN3 hardware ID', '硬體 ID 分壓（上）': 'Hardware ID divider (top)', 'V1.3 起': 'from V1.3',
    '硬體 ID 分壓（下）': 'Hardware ID divider (bottom)', '輸出電壓分壓上臂': 'Output voltage divider (top)', '115 kΩ ×3（V1.3）': '115 kΩ ×3 (V1.3)',
    '輸出電壓分壓下臂': 'Output voltage divider (bottom)', '9.09 kΩ（V1.3）': '9.09 kΩ (V1.3)', '120V 量測端': '120 V sense', '接充電輸出 +': 'to charge output +',
    '接充電輸出 −': 'to charge output −', 'ESP32-S3 模組': 'ESP32-S3 module', 'PSU UART 排針': 'PSU UART header',
    'GND / TXD0 / RXD0 — 裝在板子背面（虛線框）': 'GND / TXD0 / RXD0 — on the back (dashed)', 'OLED 排針': 'OLED header',
    'I²C（SDA / SCL / 3.3V）→ 疊在 ESP32 上方的 0.96″ SSD1306': 'I²C (SDA / SCL / 3.3 V) → 0.96″ SSD1306 stacked over the ESP32',
    '燒錄與序列埠': 'Flashing / serial', '3.3V 穩壓': '3.3 V regulator', '5V 穩壓': '5 V regulator', '5V 降壓': '5 V buck', '降壓電感': 'Buck inductor',
    '電路板圖只在原廠硬體上顯示；這台的數值照常列在右側。': 'The board drawing is shown on genuine hardware only; this unit\'s values are listed as usual.',
    '經 H1 排針接 I²C（0x3C），': 'on I²C (0x3C) via H1, ', '疊在 ESP32-S3 模組上方；位置取自外殼上蓋的顯示窗': 'stacked over the ESP32-S3; position from the lid\'s display window',
    '進入測試模式後點這裡可以切換': 'Tap here to toggle in test mode',
    '狀態機不在待機（充電流程進行中、故障或緊急停止）': 'Not in standby (charging, fault or emergency stop)', '緊急停止鎖存中 —— 先在設定選單 Reset Fault': 'Emergency stop latched — Reset Fault in the menu first',
    'CP 有電壓 —— 有車接著': 'CP has voltage — a vehicle is connected', '3 秒內收到車端 CAN —— 有車接著': 'Vehicle CAN seen in the last 3 s — a vehicle is connected',
    '這個頁面超過 10 秒沒有回應（關閉、鎖屏或斷線）': 'This page did not respond for 10 s (closed, locked or disconnected)', '手動結束': 'Ended by user',
    '另一個頁面進入了測試模式': 'Another page entered test mode', '測試模式已經結束': 'Test mode has ended', '● 手動控制中': '● Manual control',
    '—— 狀態機的輸出暫停。按 START、接上車、按緊急停止或關掉這頁，輸出都會立刻交回狀態機。': ' — the state machine\'s outputs are paused. START, plugging in a vehicle, the emergency button or closing this page hands them back at once.',
    '另一個頁面正在測試模式中。': 'Another page is in test mode.', '現在不能進入：': 'Cannot enter now: ',
    '只在沒接車、待機時可用。進入後可手動切換下面的輸出，用來驗板子。': 'Only in standby without a vehicle. Lets you switch the outputs below by hand to check a board.',
    '上次結束原因：': 'Last ended: ', '結束測試模式': 'End test mode', '改由這個頁面接管': 'Take over from this page', '送出失敗：': 'Send failed: ',
    'DC 主繼電器閉合後，充電槍的 DC+／DC− 端子會直接帶 PSU 的輸出電壓。': 'With the DC relay closed, the connector\'s DC+/DC− carry the PSU output voltage.',
    '確認 PSU 已關閉，或槍頭沒有外露、沒有人碰得到，再繼續。': 'Make sure the PSU is off, or the connector is covered and nobody can touch it, before continuing.',
    '（閉合時「120V 量測端」讀到的就是 PSU 電壓，可以拿三用電表對照分壓係數。）': '(While closed, the 120 V sense reads the PSU voltage — compare with a meter to check the divider.)',
    '測試模式會暫時由這個頁面控制繼電器、電子鎖、VP 與三顆 LED，全部從「關」開始。': 'Test mode lets this page control the relay, lock, VP and the three LEDs, all starting off.',
    '以下任一情況會立即結束，輸出交還給狀態機：': 'It ends at once, handing outputs back, on any of:', '・按下 START（或遠端啟動）': '・START (or a remote start)',
    '・接上車輛（CP 有電壓或收到 CAN）': '・a vehicle connected (CP voltage or CAN)', '・按下緊急停止': '・the emergency button', '・這個頁面 10 秒沒有回應': '・this page not responding for 10 s',
    '要進入嗎？': 'Enter?', 'OFF（無車）': 'OFF (no vehicle)', 'ON（車端就緒）': 'ON (vehicle ready)', '異常電壓': 'Abnormal voltage', '停止': 'Stop', '復原中': 'Recovering',
    '可控整流器': 'Controllable rectifier', '量測節點': 'Measurement node', 'DC 主繼電器': 'DC relay', '斷開': 'Open', '鎖上': 'Locked', '輸出': 'Output', '按下': 'Pressed',
    '緊急停止鎖存': 'Emergency latch', '鎖存中': 'Latched', 'CP 電壓': 'CP voltage', 'CP 判定': 'CP state', '輸出電壓（ADC）': 'Output voltage (ADC)',
    '輸出電壓（PSU 回報）': 'Output voltage (PSU)', '硬體版本': 'Hardware', '不認得': 'Unknown', '版本來源': 'Identified by', '出廠紀錄': 'Factory record',
    'AIN3 硬體 ID': 'AIN3 hardware ID', 'V（級': ' V (level ', '，與出廠紀錄不符 → 檢查 R36／R37': ', does not match the factory record → check R36/R37', '分壓係數': 'Divider ratio',
    'I²C 讀取失敗': 'I²C read failures', '（增加中）': ' (rising)', '傳輸': 'Transport', 'UART（H2 排針）': 'UART (H2 header)', '連線': 'Link', '節點': 'Node',
    '尚未收到 CAP': 'No CAP yet', '電壓 / 電流': 'Voltage / current', '最後 ST': 'Last ST', '收到訊框': 'Frames received', 'CRC 錯誤': 'CRC errors', '無法辨識': 'Unrecognised',
    'CAN ● 收到車端': 'CAN ● vehicle seen', 'CAN 無車端訊號': 'CAN: no vehicle', '控制器': 'Controller', 'TX / RX 錯誤計數': 'TX / RX error counters', '最後 0x500': 'Last 0x500',
    '收 / 送': 'RX / TX', '狀態機': 'State machine', '開機時間': 'Uptime', '上次重開原因': 'Last reset reason', '出廠認證': 'Factory check', '原廠': 'Genuine', '無效（': 'Invalid (',
    '無回應（I²C 0x3C）': 'No response (I²C 0x3C)', '晶片溫度': 'Chip temperature', '內部 RAM 可用': 'Internal RAM free', '內部 RAM 歷史最低': 'Internal RAM lowest',
    '最大連續區塊': 'Largest block', 'PSRAM 可用': 'PSRAM free', 'WiFi 訊號': 'WiFi signal', '未連線（AP 模式？）': 'Not connected (AP mode?)',
    '使用中 / TIME_WAIT / 上限': 'Active / TIME_WAIT / max', '監聽中': 'Listening', '網頁伺服器 socket': 'Web server sockets', '狀態': 'State', '對方': 'Peer',
    '佇列': 'Queue', '可寫': 'Writable', '重傳': 'Retrans.', 'RTO / 閒置': 'RTO / idle', '已結束': 'Ended', '● 連線中斷': '● Disconnected', '● 即時': '● Live',
    '硬體狀態 —': 'Hardware status —', '韌體': 'Firmware',

    // ── monitor.html（GitHub Pages 的 Cloud PWA）──
    'TES 遠端監控': 'TES Remote', 'TES監控': 'TES Remote', '📡 TES 遠端監控設定': '📡 TES remote monitor setup',
    'iPhone／iPad：先加入主畫面再設定': 'iPhone/iPad: add to the home screen first, then set up',
    'Safari 下方「分享」→「加入主畫面」，之後從主畫面的圖示打開。': 'In Safari tap Share → Add to Home Screen, then open it from the home-screen icon.',
    '主畫面的 App 和 Safari 各存各的：在 Safari 這裡填好的帳密，主畫面那個看不到，要在那邊再填一次。':
      'The home-screen app and Safari keep separate storage: credentials entered here in Safari are not visible there, so enter them again there.',
    '⚙ MQTT 連線設定': '⚙ MQTT connection', 'WebSocket Port（wss）': 'WebSocket port (wss)', '帳號（broker 需要時）': 'Username (if the broker needs one)', '密碼': 'Password',
    '這個頁面是 HTTPS，只能用加密的 WebSocket（wss）。常用埠：': 'This page is HTTPS, so only encrypted WebSocket (wss) works. Common ports: ',
    'EMQX Cloud（': 'EMQX Cloud (', '）、': '), ', 'HiveMQ Cloud（': 'HiveMQ Cloud (', 'Mosquitto（': 'Mosquitto (', '）→': ') →',
    '帳號密碼只存在這個瀏覽器，不會放進分享連結。': 'Credentials stay in this browser and are never put in the share link.', '🔌 連接': '🔌 Connect',
    '透過 MQTT 遠端控制充電器': 'Control the charger remotely over MQTT', '請填寫 Broker 和 Topic': 'Fill in the broker and topic', '已連線 {}': 'Connected {}',
    '已斷線，重連中…': 'Disconnected, reconnecting…', '離線，重連中…': 'Offline, reconnecting…', '連線錯誤：': 'Connection error: ',
    '✗ 尚未連線，無法發送指令': '✗ Not connected, cannot send', '遠端開始充電？': 'Start charging remotely?',
    '請確認充電槍已經插在車上——車子要完成交握才會真的送電。': 'Make sure the connector is in the vehicle — power only flows after the handshake.',
    '已送出開始指令，等充電樁回報（約 10 秒內）…': 'Start sent, waiting for the charger (about 10 s)…', '已送出停止指令，等充電樁回報…': 'Stop sent, waiting for the charger…',
    '充電樁沒有開「允許透過 MQTT 遠端啟停」。': 'The charger has "Allow remote start/stop over MQTT" off.',
    '在家裡 WiFi 下打開充電樁網頁的設定，打開後重新啟動。': 'On your home WiFi, turn it on in the charger\'s settings page, then restart it.',
    '透過 MQTT 遠端控制充電器。': 'Control the charger remotely over MQTT.',
    '充電樁要開「允許透過 MQTT 遠端啟停」（改了要重新啟動），沒開的話按了不會有反應。': 'The charger needs "Allow remote start/stop over MQTT" on (restart after changing it); otherwise the buttons do nothing.',
    '✗ 充電樁沒有反應。可能沒開「允許透過 MQTT 遠端啟停」，或開了還沒重新啟動': '✗ No reaction from the charger. "Allow remote start/stop over MQTT" may be off, or on but not yet restarted',
    '✗ 充電樁沒有反應': '✗ No reaction from the charger', '這個 broker 需要帳號密碼，填好後按「連接」': 'This broker needs a username and password — fill them in and press Connect'
  };

  // ── 比對 ──
  var CJK = /[　-〿一-鿿＀-￯]/;
  var norm = function (s) { return s.replace(/\s+/g, ' ').trim(); };
  var exact = {}, pats = [], subs = [], loose = [];
  Object.keys(EN).forEach(function (k) {
    var n = norm(k);
    if (k.indexOf('{}') >= 0) {
      var re = n.split('{}').map(function (p) { return p.replace(/[.*+?^$()|[\]\\]/g, '\\$&'); }).join('(.+?)');
      pats.push([new RegExp('^' + re + '$'), EN[k]]);
      // 長句裡的片段（例如兩個樣式接在一起的提示）：{} 只吃不含空白與全形標點的一段，避免吃過頭
      var stat = n.replace(/\{\}/g, '').trim();
      if (typeof EN[k] === 'string' && CJK.test(stat) && stat.length >= 3)
        loose.push([new RegExp(re.replace(/\(\.\+\?\)/g, '([^\\s\uff08\uff09\uff0c\uff1a\u3002]+?)'), 'g'), EN[k]]);
    } else {
      exact[n] = EN[k];
      if (n.length >= 2 && CJK.test(n)) subs.push([n, EN[k]]);
    }
  });
  subs.sort(function (a, b) { return b[0].length - a[0].length; });   // 長的先換

  function fill(v, g) {
    if (typeof v === 'function') return v.apply(null, g);
    var j = 0;
    return v.replace(/\{\}/g, function () { return g[j++]; });
  }

  function tr(s) {
    if (!s || !CJK.test(s)) return s;
    var lead = s.match(/^\s*/)[0], tail = s.match(/\s*$/)[0], n = norm(s);
    if (exact[n] !== undefined) {   // 原文前後已有空白時，譯文自己的前後空白不要再加一次
      var e = exact[n];
      if (lead) e = e.replace(/^\s+/, '');
      if (tail) e = e.replace(/\s+$/, '');
      return lead + e + tail;
    }
    // 樣式：抓到的部分都沒有中文的，直接用；否則每個對得上的都試（抓到的部分再翻一次），
    // 挑翻完剩最少中文字的，一樣時用 {} 少的（最外層）。例：「剩 2 時 30 分」套「剩 {}」
    var best = null, bestLeft = 1e9, bestN = 1e9;
    for (var i = 0; i < pats.length; i++) {
      var m = n.match(pats[i][0]);
      if (!m) continue;
      var g = m.slice(1);
      if (!g.some(function (x) { return CJK.test(x); })) return lead + fill(pats[i][1], g) + tail;
      var cand = fill(pats[i][1], g.map(tr)), left = (cand.match(/[一-鿿]/g) || []).length;
      if (left < bestLeft || (left === bestLeft && g.length < bestN)) { best = cand; bestLeft = left; bestN = g.length; }
    }
    if (best !== null) return lead + best + tail;   // 剩下的中文是抓到的資料（例如裝置名稱），照原樣
    // 長句：多行（alert）逐行，其餘把認得的片段換掉
    if (s.indexOf('\n') >= 0) return s.split('\n').map(tr).join('\n');
    var out = s;
    for (var q = 0; q < loose.length && CJK.test(out); q++)
      out = out.replace(loose[q][0], function () {
        var a = arguments, j = 1;
        return loose[q][1].replace(/\{\}/g, function () { return a[j++]; });
      });
    for (var k = 0; k < subs.length && CJK.test(out); k++)
      if (out.indexOf(subs[k][0]) >= 0) out = out.split(subs[k][0]).join(subs[k][1]);
    // 剩下的全形標點（組字串時夾在中間的）換成半形
    out = out.replace(/\u3000/g, ' ').replace(/\uff08/g, ' (').replace(/\uff09/g, ')')
             .replace(/\uff1a/g, ': ').replace(/\uff0c/g, ', ').replace(/\u3002/g, '. ');
    // \u9084\u6709\u4e2d\u6587\u5b57\u6c92\u7ffb\u5230\uff1a\u6574\u6bb5\u7dad\u6301\u4e2d\u6587\u3002\u534a\u4e2d\u534a\u82f1\u6bd4\u54ea\u4e00\u7a2e\u90fd\u96e3\u8b80
    return /[\u4e00-\u9fff]/.test(out) ? s : out;
  }

  var ATTRS = ['placeholder', 'title', 'aria-label'];
  function apply(root) {
    if (!root) return;
    if (root.nodeType === 3) { var v = tr(root.nodeValue); if (v !== root.nodeValue) root.nodeValue = v; return; }
    if (root.nodeType !== 1 || root.tagName === 'SCRIPT' || root.tagName === 'STYLE') return;
    ATTRS.forEach(function (a) {
      var v = root.getAttribute && root.getAttribute(a);
      if (v && CJK.test(v)) { var t = tr(v); if (t !== v) root.setAttribute(a, t); }   // 沒變就別寫：寫了又觸發觀察者，無限迴圈
    });
    for (var c = root.firstChild; c; c = c.nextSibling) apply(c);
  }

  // ── 切換鈕：放進頁面標題列的 #lang-sw；頁面沒留位置才固定在右上角。切換會重新載入（翻譯是單向的） ──
  function addSwitch() {
    var slots = document.querySelectorAll('#lang-sw, .lang-sw');
    if (!slots.length) { addOne(null); return; }
    for (var i = 0; i < slots.length; i++) addOne(slots[i]);
  }
  function addOne(slot) {
    var b = document.createElement('button');
    b.textContent = lang === 'zh' ? 'EN' : '中';
    b.title = lang === 'zh' ? 'English' : '中文';
    b.setAttribute('aria-label', lang === 'zh' ? 'Switch to English' : '切換成中文');
    b.style.cssText = (slot ? '' : 'position:fixed;top:8px;right:8px;z-index:9999;') +
      'padding:2px 8px;border-radius:10px;border:1px solid #444;background:#1c1c1c;color:#bbb;' +
      'font-size:12px;line-height:1.4;cursor:pointer;font-weight:normal;vertical-align:middle';
    b.onclick = function () { I18N.set(lang === 'zh' ? 'en' : 'zh'); };
    (slot || document.body).appendChild(b);
  }

  window.I18N = {
    lang: lang,
    t: function (s) { return lang === 'en' ? tr(s) : s; },
    locale: lang === 'en' ? 'en-GB' : 'zh-TW',   // 日期時間格式
    set: function (l) { try { localStorage.setItem(KEY, l); } catch (e) { /* 無痕模式：只影響這一次 */ } location.reload(); }
  };

  if (lang === 'en') {
    var _alert = window.alert, _confirm = window.confirm;
    window.alert = function (m) { return _alert.call(window, tr(String(m))); };
    window.confirm = function (m) { return _confirm.call(window, tr(String(m))); };
    document.documentElement.lang = 'en';
  }

  function start() {
    addSwitch();
    if (lang !== 'en') return;
    document.title = tr(document.title);
    // 頁面之後才改的標題（例如換成裝置名稱）也要翻
    var tt = document.querySelector('title');
    if (tt) new MutationObserver(function () {
      var v = tr(document.title); if (v !== document.title) document.title = v;
    }).observe(tt, { childList: true, characterData: true, subtree: true });
    apply(document.body);
    new MutationObserver(function (list) {
      list.forEach(function (m) {
        if (m.type === 'characterData') apply(m.target);
        else if (m.type === 'attributes') apply(m.target);
        else m.addedNodes.forEach(apply);
      });
    }).observe(document.body, { childList: true, subtree: true, characterData: true, attributes: true, attributeFilter: ATTRS });
  }
  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', start);
  else start();
})();
