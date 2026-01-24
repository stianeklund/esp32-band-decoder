#include "include/html_content.h"

#include <cstring>
#include <sstream>
#include <vector>
#include <algorithm>
#include <esp_log.h>

const char *HtmlContent::TAG = "HTML";

const char *HtmlContent::HTML_HEADER = R"(<!DOCTYPE html><html lang="en"><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1.0,maximum-scale=1.0,user-scalable=no"><title>Antenna Switch Controller</title><script>(function(){const savedTheme=localStorage.getItem('theme')||(window.matchMedia('(prefers-color-scheme: dark)').matches?'dark':'light');document.documentElement.setAttribute('data-theme',savedTheme);})();</script><style>:root[data-theme="light"]{--primary-color:#3498db;--secondary-color:#2c3e50;--page-background-color:#ecf0f1;--text-color:#34495e;--border-color:#bdc3c7;--card-background-color:#fff;--input-background-color:#fff;--input-border-color:#bdc3c7;--button-default-bg-color:#e0e0e0;--button-default-border-color:#ddd;--th-text-color:#fff;color-scheme:light}:root[data-theme="dark"]{--primary-color:#4a9eff;--secondary-color:#a8c7fa;--page-background-color:#121212;--text-color:#e0e0e0;--border-color:#2d2d2d;--card-background-color:#1e1e1e;--input-background-color:#2d2d2d;--input-border-color:#404040;--button-default-bg-color:#2d2d2d;--button-default-border-color:#404040;--th-text-color:#fff;color-scheme:dark}body{font-family:'Segoe UI',Tahoma,Geneva,Verdana,sans-serif;line-height:1.6;color:var(--text-color);max-width:1200px;margin:0 auto;padding:15px;background-color:var(--page-background-color)}h1,h2{color:var(--secondary-color);text-align:center;margin-bottom:30px}.status-container{display:flex;flex-direction:column;gap:20px;margin-bottom:30px}.status-box{background-color:var(--card-background-color);border-radius:10px;padding:15px;box-shadow:0 4px 6px rgba(0,0,0,.1);width:100%;transition:transform .3s ease}.status-box:hover{transform:translateY(-5px)}table{width:100%;border-collapse:separate;border-spacing:0;margin-bottom:20px;border-radius:10px;overflow-x:auto}th,td{padding:15px;text-align:left;border-bottom:1px solid var(--border-color)}th{font-weight:bold;color:var(--th-text-color);background-color:var(--primary-color)}tr:last-child td{border-bottom:none}.relay-grid{display:grid;grid-template-columns:repeat(2,1fr);gap:10px}.relay-button{padding:12px 8px;border:1px solid var(--button-default-border-color);border-radius:8px;background-color:var(--button-default-bg-color);color:var(--text-color);cursor:pointer;transition:all .3s ease;font-weight:bold;font-size:14px}.relay-button.active{background-color:#2ecc71;color:white;border-color:#27ae60}.relay-button.multi-band{background-color:var(--primary-color);color:white;border-color:var(--border-color)}.relay-button.multi-band.active{background-color:#2ecc71}.relay-button:hover{transform:translateY(-2px);box-shadow:0 4px 6px rgba(0,0,0,.1)}.relay-button.transmitting,.relay-button.active.transmitting,.relay-button.multi-band.active.transmitting{background-color:#e74c3c!important;color:white!important;border-color:#c0392b!important}.button-container{display:flex;flex-wrap:wrap;gap:10px;justify-content:center}.button,input[type="submit"]{width:100%;max-width:300px;margin:5px 0;display:inline-block;background-color:var(--primary-color);color:white;padding:12px 24px;border-radius:25px;text-decoration:none;transition:all .3s ease;border:none;cursor:pointer;font-size:16px;font-weight:bold;text-transform:uppercase;letter-spacing:1px}.button:hover,input[type="submit"]:hover{background-color:var(--secondary-color);transform:translateY(-2px);box-shadow:0 4px 6px rgba(0,0,0,.1)}.config-form{background-color:var(--card-background-color);padding:15px;border-radius:10px;box-shadow:0 4px 6px rgba(0,0,0,.1);color:var(--text-color);border:1px solid var(--input-border-color);margin-bottom:20px}.form-group{margin-bottom:15px}label{display:block;margin-bottom:8px;font-weight:bold;color:var(--secondary-color)}input[type="text"],input[type="number"],select{width:100%;padding:10px;margin:5px 0;display:inline-block;background-color:var(--input-background-color);color:var(--text-color);border:1px solid var(--input-border-color);border-radius:4px;box-sizing:border-box;transition:border-color .3s ease}input[type="text"]:focus,input[type="number"]:focus,select:focus{border-color:var(--primary-color);outline:none}input[type="checkbox"]{margin-right:5px;accent-color:var(--primary-color)}input[type="radio"]{accent-color:var(--primary-color)}.relay-groups{display:flex;flex-direction:column;gap:20px}.relay-group{background-color:var(--card-background-color);border-radius:8px;padding:15px}.relay-group h3{margin:0 0 15px 0;color:var(--secondary-color);text-align:center}.auto-mode-container{background-color:var(--card-background-color);border-radius:10px;padding:20px;box-shadow:0 4px 6px rgba(0,0,0,.1);margin-bottom:30px;transition:all .3s ease}.auto-mode-container h2{margin-top:0;color:var(--secondary-color)}.auto-mode-container label{display:flex;align-items:center;font-weight:normal;color:var(--text-color)}.auto-mode-container input[type="checkbox"]{margin-right:10px}@media (min-width:768px){body{padding:20px}.status-container{flex-direction:row}.status-box{width:48%}.relay-grid{grid-template-columns:repeat(4,1fr)}.button,input[type="submit"]{width:auto}}.config-form table{background-color:var(--card-background-color);color:var(--text-color);border:1px solid var(--input-border-color);margin:20px 0;width:100%;border-radius:8px}@media (max-width:767px){table th,table td{display:block;width:100%;box-sizing:border-box}table tr{margin-bottom:15px;display:block}table td{border-top:none}.config-form table{display:block;overflow-x:auto}.config-form table th,.config-form table td{min-width:120px}}@media (hover:none){.relay-button{min-height:44px}.button,input[type="submit"],select{min-height:44px;touch-action:manipulation}input[type="checkbox"]{min-width:22px;min-height:22px}}.theme-toggle{position:fixed;top:20px;right:20px;padding:12px 24px;border-radius:25px;background-color:var(--primary-color);color:white;border:none;cursor:pointer;box-shadow:0 2px 5px rgba(0,0,0,.2);transition:all .3s ease;font-size:16px;font-weight:bold;text-transform:uppercase;letter-spacing:1px;z-index:1000}.theme-toggle:hover{background-color:var(--secondary-color);transform:translateY(-2px);box-shadow:0 4px 6px rgba(0,0,0,.1)}[data-theme="dark"] body{background-color:var(--page-background-color)}[data-theme="dark"] .status-box,[data-theme="dark"] .relay-group,[data-theme="dark"] .auto-mode-container{background-color:var(--card-background-color)}[data-theme="dark"] .config-form{background-color:var(--card-background-color);border-color:var(--input-border-color)}[data-theme="dark"] .config-form table{background-color:var(--card-background-color);border-color:var(--input-border-color)}[data-theme="dark"] input[type="text"],[data-theme="dark"] input[type="number"],[data-theme="dark"] select{background-color:var(--input-background-color);color:var(--text-color);border-color:var(--input-border-color)}[data-theme="dark"] .relay-button{background-color:var(--button-default-bg-color);color:var(--text-color);border-color:var(--button-default-border-color)}[data-theme="dark"] .relay-button.active{background-color:#2ecc71;color:white}[data-theme="dark"] .relay-button.multi-band{background-color:#1a5f89;color:white;border-color:#2980b9}[data-theme="dark"] .relay-button.multi-band.active{background-color:#2ecc71;color:white}.relay-button.rx-antenna{border:2px dashed #9b59b6;position:relative}.relay-button.rx-antenna::after{content:'RX';position:absolute;top:2px;right:4px;font-size:10px;color:#9b59b6;font-weight:bold}.relay-button.rx-antenna.active{background-color:#9b59b6;color:white;border-style:solid;border-color:#8e44ad}.relay-button.rx-antenna.active::after{color:white}[data-theme="dark"] .relay-button.rx-antenna{border-color:#9b59b6}[data-theme="dark"] .relay-button.rx-antenna::after{color:#bb7de0}[data-theme="dark"] .relay-button.rx-antenna.active{background-color:#9b59b6;border-color:#8e44ad}[data-theme="dark"] .relay-button.rx-antenna.active::after{color:white}.toast{position:fixed;bottom:20px;left:50%;transform:translateX(-50%);background-color:#333;color:white;padding:12px 24px;border-radius:8px;z-index:1001;opacity:0;transition:opacity .3s ease}.toast.show{opacity:1}</style></head><body><button class="theme-toggle" onclick='toggleTheme()' type="button">Theme</button>
)";

// ReSharper disable once CppUseAuto
const char *HtmlContent::HTML_FOOTER = R"(
</body>
</html>
)";

const std::map<std::string_view, HtmlContent::BandInfo> HtmlContent::band_info = {
    {"160m", {"160m", 1800000, 2000000}},
    {"80m", {"80m", 3500000, 4000000}},
    {"40m", {"40m", 7000000, 7300000}},
    {"30m", {"30m", 10100000, 10150000}},
    {"20m", {"20m", 14000000, 14350000}},
    {"17m", {"17m", 18068000, 18168000}},
    {"15m", {"15m", 21000000, 21450000}},
    {"12m", {"12m", 24890000, 24990000}},
    {"10m", {"10m", 28000000, 29700000}},
    {"6m", {"6m", 50000000, 54000000}},
    // {"4m", {"4m", 70000000, 74000000}},
    // {"2m", {"2m", 144000000, 148000000}},
    // {"70cm", {"70cm", 420000000, 450000000}}
};

// Helper function to get bands sorted by frequency (high to low)
std::vector<std::pair<std::string_view, HtmlContent::BandInfo>> HtmlContent::get_bands_by_frequency() {
    std::vector<std::pair<std::string_view, BandInfo>> sorted_bands(band_info.begin(), band_info.end());
    
    // Sort by start frequency, high to low
    std::sort(sorted_bands.begin(), sorted_bands.end(), 
        [](const auto& a, const auto& b) {
            return a.second.start_freq > b.second.start_freq;
        });
    
    return sorted_bands;
}

std::string HtmlContent::generate_root_html(const antenna_switch_config_t &config, const char *ip_addr, const char *mac_addr) {
    std::stringstream ss;
    ss << HtmlContent::HTML_HEADER;
    ss << "<h1>Antenna Controller</h1>";
    ss << "<div class='status-container'>";
    ss << "<div class='status-box'>";
    ss << "<h2>Current Status (Radio A)</h2>"; // Clarify this is for Radio A
    ss << "<table>";
    ss << "<tr><th>Frequency</th><td id='current-frequency'>Updating...</td></tr>";
    ss << "<tr><th>Port</th><td><span id='active-antenna'>Updating...</span></td></tr>";
    ss << "<tr><th>Data Source</th><td><span id='data-source'>Updating...</span></td></tr>";
    ss << "</table>";
    ss << "</div>"; // End of Radio A status-box

// Conditionally add Radio B status box:
    if (config.radio_operation_mode != RADIO_OP_MODE_SINGLE_A) {
        ss << "<div class='status-box'>";
        ss << "<h2>Current Status (Radio B)</h2>";
        ss << "<table>";
        ss << "<tr><th>Frequency</th><td id='current-frequency-b'>Updating...</td></tr>";
        ss << "<tr><th>Port</th><td><span id='active-antenna-b'>Updating...</span></td></tr>";
        ss << "<tr><th>Data Source</th><td><span id='data-source-b'>Updating...</span></td></tr>";
        ss << "</table>";
        ss << "</div>"; // End of Radio B status-box
    }

    ss << R"(
        <div class="status-box">
            <h2>Network Information</h2>
            <table>
                <tr><th>IP Address</th><td>)" << ip_addr << R"(</td></tr>
                <tr><th>MAC Address</th><td>)" << mac_addr << R"(</td></tr>
            </table>
            <div style="margin-top: 15px; text-align: center;">
                <form action='/reset-wifi' method='post' style='display:inline' onsubmit='return confirm("Reset WiFi credentials?")'>
                    <button type="submit" class="button warning">Reset WiFi</button>
                </form>
            </div>
        </div>
    </div>
    <div class="status-box" style="width: 100%; margin-top: 20px;">
        <h2>Relay Controls</h2>
        <div class="relay-groups">
            <div class="relay-group">
                <h3>Radio A</h3>
                <div class="relay-grid">)";

// This is only really applicable to the KC868-A16 or any other configuration with two groups of 8 outputs

// First 8 relays (Radio A)
for (int i = 0; i < 8; i++) {
    // Ensure the relay name is properly escaped for HTML and use default if empty
    std::string relay_name;
    if (config.relay_names[i][0] != '\0') {
        relay_name = config.relay_names[i];
    } else {
        relay_name = "Relay " + std::to_string(i + 1);
    }
    ss << "<button class='relay-button' data-relay='" << (i + 1) << "' onclick='toggleRelay(" << (i + 1) << ")'>"
       << relay_name << "</button>";
}

// Conditionally render Radio B block
if (config.radio_operation_mode != RADIO_OP_MODE_SINGLE_A) {
    ss << R"(
                </div>
            </div>
            <div class="relay-group">
                <h3>Radio B</h3>
                <div class="relay-grid">)";
    for (int i = 8; i < 16; i++) {
        // Ensure the relay name is properly escaped for HTML and use default if empty
        std::string relay_name;
        if (config.relay_names[i][0] != '\0') {
            relay_name = config.relay_names[i];
        } else {
            relay_name = "Relay " + std::to_string(i + 1);
        }
        ss << "<button class='relay-button' data-relay='" << (i + 1) << "' onclick='toggleRelay(" << (i + 1) << ")'>"
           << relay_name << "</button>";
    }
    ss << R"(
                </div>
            </div>
    )";
} else {
    ss << R"(
                </div>
            </div>
    )";
}

ss << R"(
        </div>
    </div>
    <div class="button-container">
        <a href='/config' class="button">Edit Configuration</a>
        <form action='/restart' method='post' style='display:inline' onsubmit='handleRestart(event)'>
            <button type="submit" class="button" style="background-color:#e74c3c">Restart Device</button>
        </form>
<script>function handleRestart(e){if(!confirm('Are you sure you want to restart the device?')){e.preventDefault();return false;}const b=e.target.querySelector('button');b.textContent='Restarting...';b.disabled=true;setTimeout(()=>{document.body.innerHTML='<h1 style="text-align:center;margin-top:50px;">Device is restarting...</h1><p style="text-align:center">This page will refresh in 10 seconds.</p>';setTimeout(()=>{window.location.reload();},10000);},500);return true;}</script>
    </div>
<script>function setTheme(t){document.documentElement.setAttribute('data-theme',t);localStorage.setItem('theme',t);}function toggleTheme(){const c=document.documentElement.getAttribute('data-theme')||'light';setTheme(c==='light'?'dark':'light');}setTheme(localStorage.getItem('theme')||(window.matchMedia('(prefers-color-scheme: dark)').matches?'dark':'light'));
let isRelayOp=false;const COOLDOWN=250,INTERVAL=2000;async function toggleRelay(r){if(isRelayOp)return;try{isRelayOp=true;const b=document.querySelector(`button[data-relay="${r}"]`);b.disabled=true;const s=!b.classList.contains('active');const res=await fetch('/relay/control',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({relay:r,state:s})});if(!res.ok){const e=await res.text();console.error('Server error:',e);throw new Error(e);}const result=await res.json();b.classList.toggle('active',result.state);if(result.state){const rb=document.querySelector(`button[data-relay="${r}"]`);if(rb&&rb.textContent.trim()&&rb.textContent.trim()!==`Relay ${r}`){document.getElementById('active-antenna').textContent=rb.textContent.trim();}else{document.getElementById('active-antenna').textContent='Relay '+r;}}fetch('/status').then(res=>res.json()).then(d=>{const aa=d.available_antennas||[];b.classList.toggle('multi-band',aa.filter(a=>a===r).length>1);});updateStatus();await new Promise(res=>setTimeout(res,COOLDOWN));}catch(e){console.error('Error toggling relay:',e);alert('Failed to toggle relay: '+e.message);}finally{document.querySelector(`button[data-relay="${r}"]`).disabled=false;isRelayOp=false;}}
let statusTO=null;async function updateRelayStatus(){if(statusTO)clearTimeout(statusTO);try{const res=await fetch('/relay/status');const d=await res.json();const states=d.states;for(let i=1;i<=16;i++){const b=document.querySelector(`button[data-relay="${i}"]`);if(b&&!b.disabled){const state=((states>>(i-1))&1)===0;b.classList.toggle('active',state);}}}catch(e){console.error('Error updating relay status:',e);}statusTO=setTimeout(updateRelayStatus,INTERVAL);}async function changeAntenna(n){try{const res=await fetch('/relay/control',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({relay:parseInt(n),state:true})});if(!res.ok)throw new Error('Failed to change antenna');document.getElementById('active-antenna').textContent='Antenna '+n;updateRelayStatus();}catch(e){console.error('Error changing antenna:',e);alert('Failed to change antenna');}}

let currentBandIndex=-1,currentBandName='',rxAntennaEnabled=false,rxAntenna=0;function updateStatus(){fetch('/status').then(res=>{if(!res.ok)throw new Error('Network error');return res.json();}).then(d=>{const fMHz=(d.frequency/1e6).toFixed(3);document.getElementById('current-frequency').textContent=fMHz+' MHz';function setAntenna(el,ant){if(!isNaN(ant.replace('Antenna ',''))&&ant.startsWith('Antenna ')){const num=parseInt(ant.replace('Antenna ',''));if(num>0&&num<=16){const rb=document.querySelector(`button[data-relay="${num}"]`);if(rb&&rb.textContent.trim()&&rb.textContent.trim()!==`Relay ${num}`){el.textContent=rb.textContent.trim();}else{el.textContent=`Relay ${num}`;}}else{el.textContent=ant;}}else{el.textContent=ant;}}setAntenna(document.getElementById('active-antenna'),d.antenna);document.getElementById('data-source').textContent=d.data_source||'Unknown';currentBandIndex=d.current_band_index?? -1;currentBandName=d.current_band_name||'';rxAntennaEnabled=d.rx_antenna_enabled||false;rxAntenna=d.rx_antenna||0;const fB=document.getElementById('current-frequency-b'),aB=document.getElementById('active-antenna-b');if(fB&&aB){if(d.hasOwnProperty('frequency_b')&&d.frequency_b>0){const fBMHz=(d.frequency_b/1e6).toFixed(3);fB.textContent=fBMHz+' MHz';}else{fB.textContent='N/A';}if(d.hasOwnProperty('antenna_b')){setAntenna(aB,d.antenna_b);}else{aB.textContent='None';}const dsB=document.getElementById('data-source-b');if(dsB)dsB.textContent=d.data_source_b||'Unknown';}document.querySelectorAll('.relay-button').forEach(b=>{const n=parseInt(b.getAttribute('data-relay'));const isTx=n<=8?d.transmitting:d.transmitting_b;const avail=n<=8?(d.available_antennas||[]):(d.available_antennas_b||[]);const supports=avail.includes(n);b.classList.remove('multi-band','transmitting','rx-antenna');if(rxAntennaEnabled&&rxAntenna===n&&!isTx){b.classList.add('rx-antenna');}if(b.classList.contains('active')){if(isTx)b.classList.add('transmitting');}else{if(supports)b.classList.add('multi-band');}});}).catch(e=>{console.error('Error:',e);['current-frequency','active-antenna','data-source','current-frequency-b','active-antenna-b','data-source-b'].forEach(id=>{const el=document.getElementById(id);if(el)el.textContent='Error updating';});});}
function showToast(msg){let t=document.querySelector('.toast');if(!t){t=document.createElement('div');t.className='toast';document.body.appendChild(t);}t.textContent=msg;t.classList.add('show');setTimeout(()=>t.classList.remove('show'),3000);}
async function handleSetRxAntenna(relayNum){if(!rxAntennaEnabled){showToast('RX Antenna feature is disabled. Enable in Configuration.');return;}if(currentBandIndex<0){showToast('No band detected. Tune to a frequency first.');return;}const btn=document.querySelector(`button[data-relay="${relayNum}"]`);const relayName=btn?btn.textContent.trim():'Relay '+relayNum;const radio=relayNum<=8?'A':'B';if(confirm(`Set "${relayName}" as RX antenna for ${currentBandName}?`)){try{const res=await fetch('/relay/set-rx-antenna',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({relay:relayNum,radio:radio,band_index:currentBandIndex})});const data=await res.json();if(data.success){showToast(data.message);updateStatus();}else{showToast('Error: '+(data.message||'Unknown error'));}}catch(e){showToast('Failed to set RX antenna');}}}
function initRxAntennaHandlers(){document.querySelectorAll('.relay-button').forEach(btn=>{btn.addEventListener('contextmenu',e=>{e.preventDefault();handleSetRxAntenna(parseInt(btn.dataset.relay));});let pressTimer;btn.addEventListener('touchstart',e=>{pressTimer=setTimeout(()=>{handleSetRxAntenna(parseInt(btn.dataset.relay));},500);},{passive:true});btn.addEventListener('touchend',()=>clearTimeout(pressTimer));btn.addEventListener('touchmove',()=>clearTimeout(pressTimer));});}
updateStatus();updateRelayStatus();setInterval(updateStatus,INTERVAL);initRxAntennaHandlers();</script>
    )";
    ss << HtmlContent::HTML_FOOTER;
    return ss.str();
}

esp_err_t HtmlContent::generate_root_html_chunked(httpd_req_t *req, const antenna_switch_config_t &config, const char *ip_addr, const char *mac_addr) {
    esp_err_t ret = ESP_OK;

    // Helper lambda to send a C-string literal as a chunk
    auto send_cstr_chunk = [&](const char* cstr_chunk) -> esp_err_t {
        if (cstr_chunk == nullptr || cstr_chunk[0] == '\0') return ESP_OK;
        const esp_err_t send_ret = httpd_resp_send_chunk(req, cstr_chunk, strlen(cstr_chunk));
        if (send_ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to send cstr chunk: %s", esp_err_to_name(send_ret));
        }
        return send_ret;
    };

    // Helper lambda to send a stringstream's content as a chunk
    auto send_ss_chunk = [&](std::stringstream& stream) -> esp_err_t {
        const std::string chunk_str = stream.str();
        stream.str(std::string()); // Clear the stringstream for reuse
        stream.clear(); // Clear error flags (like eof, fail, bad)
        if (chunk_str.empty()) return ESP_OK;
        esp_err_t send_ret = httpd_resp_send_chunk(req, chunk_str.c_str(), chunk_str.length());
        if (send_ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to send chunk: %s", esp_err_to_name(send_ret));
        }
        return send_ret;
    };

    std::stringstream ss_buffer;

    // Send HTML header
    ret = send_cstr_chunk(HtmlContent::HTML_HEADER);
    if (ret != ESP_OK) return ret;

    // Build main content in chunks
    ss_buffer << "<h1>Antenna Controller</h1>";
    ss_buffer << "<div class='status-container'>";
    ss_buffer << "<div class='status-box'>";
    ss_buffer << "<h2>Current Status (Radio A)</h2>";
    ss_buffer << "<table>";
    ss_buffer << "<tr><th>Frequency</th><td id='current-frequency'>Updating...</td></tr>";
    ss_buffer << "<tr><th>Port</th><td><span id='active-antenna'>Updating...</span></td></tr>";
    ss_buffer << "<tr><th>Data Source</th><td><span id='data-source'>Updating...</span></td></tr>";
    ss_buffer << "</table>";
    ss_buffer << "</div>";
    ret = send_ss_chunk(ss_buffer);
    if (ret != ESP_OK) return ret;

    // Radio B status box (conditional)
    if (config.radio_operation_mode != RADIO_OP_MODE_SINGLE_A) {
        ss_buffer << "<div class='status-box'>";
        ss_buffer << "<h2>Current Status (Radio B)</h2>";
        ss_buffer << "<table>";
        ss_buffer << "<tr><th>Frequency</th><td id='current-frequency-b'>Updating...</td></tr>";
        ss_buffer << "<tr><th>Port</th><td><span id='active-antenna-b'>Updating...</span></td></tr>";
        ss_buffer << "<tr><th>Data Source</th><td><span id='data-source-b'>Updating...</span></td></tr>";
        ss_buffer << "</table>";
        ss_buffer << "</div>";
        ret = send_ss_chunk(ss_buffer);
        if (ret != ESP_OK) return ret;
    }

    // Network information
    ss_buffer << R"(
        <div class="status-box">
            <h2>Network Information</h2>
            <table>
                <tr>
                    <th>IP Address</th>
                    <td>)" << ip_addr << R"(</td>
                </tr>
                <tr>
                    <th>MAC Address</th>
                    <td>)" << mac_addr << R"(</td>
                </tr>
            </table>
        </div>
    </div>
    )";
    ret = send_ss_chunk(ss_buffer);
    if (ret != ESP_OK) return ret;

    // Relay groups
    ss_buffer << "<div class='relay-groups'>";
    ss_buffer << "<div class='relay-group'>";
    ss_buffer << "<h3>Radio A Relays</h3>";
    ss_buffer << "<div class='relay-grid'>";
    
    for (int i = 0; i < 8; i++) {
        std::string relay_name;
        if (strlen(config.relay_names[i]) > 0) {
            relay_name = config.relay_names[i];
        } else {
            relay_name = "Relay " + std::to_string(i + 1);
        }
        ss_buffer << "<button class='relay-button' data-relay='" << (i + 1) << "' onclick='toggleRelay(" << (i + 1) << ")'>" << relay_name << "</button>";
    }
    ss_buffer << "</div></div>";
    ret = send_ss_chunk(ss_buffer);
    if (ret != ESP_OK) return ret;

    // Radio B relays (conditional)
    if (config.radio_operation_mode != RADIO_OP_MODE_SINGLE_A) {
        ss_buffer << "<div class='relay-group'>";
        ss_buffer << "<h3>Radio B Relays</h3>";
        ss_buffer << "<div class='relay-grid'>";
        
        for (int i = 0; i < 8; i++) {
            std::string relay_name;
            if (strlen(config.relay_names[i + 8]) > 0) {
                relay_name = config.relay_names[i + 8];
            } else {
                relay_name = config.relay_names[i]; // Mirror Radio A names
            }
            ss_buffer << "<button class='relay-button' data-relay='" << (i + 9) << "' onclick='toggleRelay(" << (i + 9) << ")'>" << relay_name << "</button>";
        }
        ss_buffer << "</div></div>";
        ret = send_ss_chunk(ss_buffer);
        if (ret != ESP_OK) return ret;
    }

    // Close relay groups and add buttons
    ss_buffer << "</div>";
    ss_buffer << "<div class='button-container'>";
    ss_buffer << "<a href='/config' class='button'>Edit Configuration</a>";
    ss_buffer << "<form action='/restart' method='post' style='display:inline' onsubmit='handleRestart(event)'>";
    ss_buffer << "<button type='submit' class='button' style='background-color:#e74c3c'>Restart Device</button>";
    ss_buffer << "</form>";
    ret = send_ss_chunk(ss_buffer);
    if (ret != ESP_OK) return ret;

    // Send JavaScript in chunks
    ss_buffer << "<script>function handleRestart(e){if(!confirm('Are you sure you want to restart the device?')){e.preventDefault();return false;}const b=e.target.querySelector('button');b.textContent='Restarting...';b.disabled=true;setTimeout(()=>{document.body.innerHTML='<h1 style=\"text-align:center;margin-top:50px;\">Device is restarting...</h1><p style=\"text-align:center\">This page will refresh in 10 seconds.</p>';setTimeout(()=>{window.location.reload();},10000);},500);return true;}</script>";
    ret = send_ss_chunk(ss_buffer);
    if (ret != ESP_OK) return ret;

    ss_buffer << "</div><script>function setTheme(t){document.documentElement.setAttribute('data-theme',t);localStorage.setItem('theme',t);}function toggleTheme(){const c=document.documentElement.getAttribute('data-theme')||'light';setTheme(c==='light'?'dark':'light');}setTheme(localStorage.getItem('theme')||(window.matchMedia('(prefers-color-scheme: dark)').matches?'dark':'light'));";
    ret = send_ss_chunk(ss_buffer);
    if (ret != ESP_OK) return ret;

    // Continue with relay and status JavaScript
    ss_buffer << "let isRelayOp=false;const COOLDOWN=250,INTERVAL=2000;async function toggleRelay(r){if(isRelayOp)return;try{isRelayOp=true;const b=document.querySelector(`button[data-relay=\"${r}\"]`);b.disabled=true;const s=!b.classList.contains('active');const res=await fetch('/relay/control',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({relay:r,state:s})});if(!res.ok){const e=await res.text();console.error('Server error:',e);throw new Error(e);}const result=await res.json();b.classList.toggle('active',result.state);if(result.state){const rb=document.querySelector(`button[data-relay=\"${r}\"]`);if(rb&&rb.textContent.trim()&&rb.textContent.trim()!==`Relay ${r}`){document.getElementById('active-antenna').textContent=rb.textContent.trim();}else{document.getElementById('active-antenna').textContent='Relay '+r;}}fetch('/status').then(res=>res.json()).then(d=>{const aa=d.available_antennas||[];b.classList.toggle('multi-band',aa.filter(a=>a===r).length>1);});updateStatus();await new Promise(res=>setTimeout(res,COOLDOWN));}catch(e){console.error('Error toggling relay:',e);alert('Failed to toggle relay: '+e.message);}finally{document.querySelector(`button[data-relay=\"${r}\"]`).disabled=false;isRelayOp=false;}}";
    ret = send_ss_chunk(ss_buffer);
    if (ret != ESP_OK) return ret;

    ss_buffer << "let statusTO=null;async function updateRelayStatus(){if(statusTO)clearTimeout(statusTO);try{const res=await fetch('/relay/status');const d=await res.json();const states=d.states;for(let i=1;i<=16;i++){const b=document.querySelector(`button[data-relay=\"${i}\"]`);if(b&&!b.disabled){const state=((states>>(i-1))&1)===0;b.classList.toggle('active',state);}}}catch(e){console.error('Error updating relay status:',e);}statusTO=setTimeout(updateRelayStatus,INTERVAL);}async function changeAntenna(n){try{const res=await fetch('/relay/control',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({relay:parseInt(n),state:true})});if(!res.ok)throw new Error('Failed to change antenna');document.getElementById('active-antenna').textContent='Antenna '+n;updateRelayStatus();}catch(e){console.error('Error changing antenna:',e);alert('Failed to change antenna');}}";
    ret = send_ss_chunk(ss_buffer);
    if (ret != ESP_OK) return ret;

    // Send the large updateStatus function with RX antenna support
    ss_buffer << "let currentBandIndex=-1,currentBandName='',rxAntennaEnabled=false,rxAntenna=0;function updateStatus(){fetch('/status').then(res=>{if(!res.ok)throw new Error('Network error');return res.json();}).then(d=>{const fMHz=(d.frequency/1e6).toFixed(3);document.getElementById('current-frequency').textContent=fMHz+' MHz';function setAntenna(el,ant){if(!isNaN(ant.replace('Antenna ',''))&&ant.startsWith('Antenna ')){const num=parseInt(ant.replace('Antenna ',''));if(num>0&&num<=16){const rb=document.querySelector(`button[data-relay=\"${num}\"]`);if(rb&&rb.textContent.trim()&&rb.textContent.trim()!==`Relay ${num}`){el.textContent=rb.textContent.trim();}else{el.textContent=`Relay ${num}`;}}else{el.textContent=ant;}}else{el.textContent=ant;}}setAntenna(document.getElementById('active-antenna'),d.antenna);document.getElementById('data-source').textContent=d.data_source||'Unknown';currentBandIndex=d.current_band_index?? -1;currentBandName=d.current_band_name||'';rxAntennaEnabled=d.rx_antenna_enabled||false;rxAntenna=d.rx_antenna||0;const fB=document.getElementById('current-frequency-b'),aB=document.getElementById('active-antenna-b');if(fB&&aB){if(d.hasOwnProperty('frequency_b')&&d.frequency_b>0){const fBMHz=(d.frequency_b/1e6).toFixed(3);fB.textContent=fBMHz+' MHz';}else{fB.textContent='N/A';}if(d.hasOwnProperty('antenna_b')){setAntenna(aB,d.antenna_b);}else{aB.textContent='None';}const dsB=document.getElementById('data-source-b');if(dsB)dsB.textContent=d.data_source_b||'Unknown';}document.querySelectorAll('.relay-button').forEach(b=>{const n=parseInt(b.getAttribute('data-relay'));const isTx=n<=8?d.transmitting:d.transmitting_b;const avail=n<=8?(d.available_antennas||[]):(d.available_antennas_b||[]);const supports=avail.includes(n);b.classList.remove('multi-band','transmitting','rx-antenna');if(rxAntennaEnabled&&rxAntenna===n&&!isTx){b.classList.add('rx-antenna');}if(b.classList.contains('active')){if(isTx)b.classList.add('transmitting');}else{if(supports)b.classList.add('multi-band');}});}).catch(e=>{console.error('Error:',e);['current-frequency','active-antenna','data-source','current-frequency-b','active-antenna-b','data-source-b'].forEach(id=>{const el=document.getElementById(id);if(el)el.textContent='Error updating';});});}";
    ret = send_ss_chunk(ss_buffer);
    if (ret != ESP_OK) return ret;

    // RX antenna handler functions
    ss_buffer << "function showToast(msg){let t=document.querySelector('.toast');if(!t){t=document.createElement('div');t.className='toast';document.body.appendChild(t);}t.textContent=msg;t.classList.add('show');setTimeout(()=>t.classList.remove('show'),3000);}async function handleSetRxAntenna(relayNum){if(!rxAntennaEnabled){showToast('RX Antenna feature is disabled. Enable in Configuration.');return;}if(currentBandIndex<0){showToast('No band detected. Tune to a frequency first.');return;}const btn=document.querySelector(`button[data-relay=\"${relayNum}\"]`);const relayName=btn?btn.textContent.trim():'Relay '+relayNum;const radio=relayNum<=8?'A':'B';if(confirm(`Set \"${relayName}\" as RX antenna for ${currentBandName}?`)){try{const res=await fetch('/relay/set-rx-antenna',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({relay:relayNum,radio:radio,band_index:currentBandIndex})});const data=await res.json();if(data.success){showToast(data.message);updateStatus();}else{showToast('Error: '+(data.message||'Unknown error'));}}catch(e){showToast('Failed to set RX antenna');}}}function initRxAntennaHandlers(){document.querySelectorAll('.relay-button').forEach(btn=>{btn.addEventListener('contextmenu',e=>{e.preventDefault();handleSetRxAntenna(parseInt(btn.dataset.relay));});let pressTimer;btn.addEventListener('touchstart',e=>{pressTimer=setTimeout(()=>{handleSetRxAntenna(parseInt(btn.dataset.relay));},500);},{passive:true});btn.addEventListener('touchend',()=>clearTimeout(pressTimer));btn.addEventListener('touchmove',()=>clearTimeout(pressTimer));});}";
    ret = send_ss_chunk(ss_buffer);
    if (ret != ESP_OK) return ret;

    // Final initialization
    ss_buffer << "updateStatus();updateRelayStatus();setInterval(updateStatus,INTERVAL);initRxAntennaHandlers();</script>";
    ret = send_ss_chunk(ss_buffer);
    if (ret != ESP_OK) return ret;

    // Send HTML footer
    ret = send_cstr_chunk(HtmlContent::HTML_FOOTER);
    if (ret != ESP_OK) return ret;

    // Send final empty chunk to signal end
    ret = httpd_resp_send_chunk(req, nullptr, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send final chunk: %s", esp_err_to_name(ret));
    }
    
    return ret;
}

esp_err_t HtmlContent::generate_config_html_chunked(httpd_req_t *req, const antenna_switch_config_t &config) {
    esp_err_t ret = ESP_OK;

    // Helper lambda to send a stringstream's content as a chunk
    auto send_ss_chunk = [&](std::stringstream& stream) -> esp_err_t {
        const std::string chunk_str = stream.str();
        stream.str(std::string()); // Clear the stringstream for reuse
        stream.clear(); // Clear error flags (like eof, fail, bad)
        if (chunk_str.empty()) return ESP_OK;
        esp_err_t send_ret = httpd_resp_send_chunk(req, chunk_str.c_str(), chunk_str.length());
        if (send_ret != ESP_OK) {
            ESP_LOGE(HtmlContent::TAG, "Failed to send chunk: %s", esp_err_to_name(send_ret));
        }
        return send_ret;
    };

    // Helper lambda to send a C-string literal as a chunk
    auto send_cstr_chunk = [&](const char* cstr_chunk) -> esp_err_t {
        if (cstr_chunk == nullptr || cstr_chunk[0] == '\0') return ESP_OK;
        const esp_err_t send_ret = httpd_resp_send_chunk(req, cstr_chunk, strlen(cstr_chunk));
         if (send_ret != ESP_OK) {
            ESP_LOGE(HtmlContent::TAG, "Failed to send cstr chunk: %s", esp_err_to_name(send_ret));
        }
        return send_ret;
    };

    // Check for potential errors before starting to generate HTML
    if (config.num_bands <= 0 || config.num_bands > MAX_BANDS) {
        ESP_LOGE(HtmlContent::TAG, "Invalid number of bands: %d (should be between 1 and %d)",
                 config.num_bands, MAX_BANDS);
        // Cannot easily send an HTTP error if chunking has started.
        // This check should ideally be done before calling this function or before sending the first chunk.
        return ESP_ERR_INVALID_ARG;
    }
    if (config.num_antenna_ports <= 0 || config.num_antenna_ports > MAX_ANTENNA_PORTS) {
        ESP_LOGE(HtmlContent::TAG, "Invalid number of antenna ports: %d (should be between 1 and %d)",
                 config.num_antenna_ports, MAX_ANTENNA_PORTS);
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGD(HtmlContent::TAG, "Generating HTML for config: %d bands, %d antenna ports", config.num_bands, config.num_antenna_ports);
    ESP_LOGD(HtmlContent::TAG, "Debug: num_bands = %d, num_antenna_ports = %d", config.num_bands, config.num_antenna_ports);
    
    std::stringstream ss_buffer; // Use this for building smaller parts

    ret = send_cstr_chunk(HtmlContent::HTML_HEADER);
    if (ret != ESP_OK) return ret;

    ss_buffer << "<h1>Configuration</h1>"; // Changed page title
    ss_buffer << "<form id='configForm' class='config-form' onsubmit='submitConfig(event)'>";
    ss_buffer << "<h2>General Device Settings</h2>";
    ss_buffer << "<div class='form-group' style='margin-bottom: 20px;'>";
    ss_buffer << "<label for='num_antenna_ports'>Number of outputs:</label>";
    ss_buffer << "<input type='number' id='num_antenna_ports' name='num_antenna_ports' value='"
            << std::to_string(config.num_antenna_ports) << "' min='1' max='" << MAX_ANTENNA_PORTS << "' onchange='updateAntennaPorts()'>";
    ss_buffer << "</div>";

// Radio Operation Mode Dropdown
    ss_buffer << "<div class='form-group'>";
    ss_buffer << "<label for='radio_operation_mode'>Radio Operation Mode:</label>";
    ss_buffer << "<select id='radio_operation_mode' name='radio_operation_mode' onchange='toggleInterlockVisibility()'>";
    ss_buffer << "<option value='SINGLE_A' " << (config.radio_operation_mode == RADIO_OP_MODE_SINGLE_A ? "selected" : "") << ">Radio A Only</option>";
    ss_buffer << "<option value='ALTERNATING_AB' " << (config.radio_operation_mode == RADIO_OP_MODE_ALTERNATING_AB ? "selected" : "") << ">Alternating (A or B, one at a time)</option>";
    ss_buffer << "<option value='CONCURRENT_AB' " << (config.radio_operation_mode == RADIO_OP_MODE_CONCURRENT_AB ? "selected" : "") << ">Concurrent (A and B, different antennas)</option>";
    ss_buffer << "</select>";
    ss_buffer << "</div>";

// Interlock Option (conditionally visible)
    ss_buffer << "<div class='form-group' id='interlock_options_div' style='display: "
       << (config.radio_operation_mode == RADIO_OP_MODE_CONCURRENT_AB ? "block" : "none") << ";'>";
    ss_buffer << "<label style='font-weight: normal;'><input type='checkbox' name='interlock_auto_resolves_conflict' "
       << (config.interlock_auto_resolves_conflict ? "checked" : "")
       << " onchange='toggleRadioBPortVisibility()'> Automatically resolve same-antenna conflict (for Concurrent mode)</label>"; // Added onchange
    
// Auto Restore Option
    ss_buffer << "<div id='auto_restore_option_div' style='margin-top: 10px; display: none;'>"; // Initially hidden, JS will manage
    ss_buffer << "<label style='font-weight: normal;'><input type='checkbox' name='auto_restore_on_conflict_resolution' "
       << (config.auto_restore_on_conflict_resolution ? "checked" : "")
       << " onchange='toggleRadioBPortVisibility()'> Automatically restore other radio's antenna after conflict is clear</label>"; // Added onchange
    ss_buffer << "</div>";

// Interlock Restore Delay
    ss_buffer << "<div class='form-group' id='interlock_restore_delay_div' style='margin-top: 10px; display: none;'>"; // Initially hidden, JS will manage
    ss_buffer << "<label for='radio_restore_delay_ms'>Interlock Restore Delay (ms):</label>";
    ss_buffer << "<input type='number' id='radio_restore_delay_ms' name='radio_restore_delay_ms' value='"
              << (config.radio_restore_delay_ms > 0 ? config.radio_restore_delay_ms : 200) // Default to 200 if 0 or uninit
              << "' min='1' max='5000' step='1'>";
    ss_buffer << "</div>";

    ss_buffer << "</div>"; // End of interlock_options_div

    // This is the end of "General Device Settings". Send the accumulated chunk.
    // Auto Mode and Concurrent Data Sources are no longer part of this chunk.
    ret = send_ss_chunk(ss_buffer);
    if (ret != ESP_OK) return ret;
    // ss_buffer is now empty.

    // num_bands div removed from here.
    // Duplicated "Switch Configuration" H2 and num_antenna_ports div removed.

    // Radio Operation Mode Dropdown
    // Duplicated Radio Operation Mode block and its send_ss_chunk are removed.

    ss_buffer << "<h2>Band & Antenna Configuration</h2>";
    // Moved "Auto Mode" checkbox here
    ss_buffer << "<div class='auto-mode-container'>";
    ss_buffer << "<label>";
    ss_buffer << "<input type='checkbox' name='auto_mode' " << (config.auto_mode ? "checked" : "") << ">";
    ss_buffer << " Enable Automatic band selection";
    ss_buffer << "</label>";
    ss_buffer << "<br>";
    ss_buffer << "<label>";
    ss_buffer << "<input type='checkbox' name='ai_mode' " << (config.ai_mode ? "checked" : "") << ">";
    ss_buffer << " Enable Auto Information (AI2)";
    ss_buffer << "</label>";
    ss_buffer << "<br>";
    ss_buffer << "<label>";
    ss_buffer << "<input type='checkbox' name='rx_antenna_enabled' " << (config.rx_antenna_enabled ? "checked" : "") << ">";
    ss_buffer << " Enable RX Antenna";
    ss_buffer << "</label>";
    ss_buffer << "<div style='margin-left: 20px; font-size: 0.9em; color: var(--text-color); opacity: 0.7;'>";
    ss_buffer << "Use a separate antenna for receiving. Right-click or long-press a relay to set it as RX antenna for the current band.";
    ss_buffer << "</div>";
    ss_buffer << "</div>";

    ss_buffer << "<div class='form-group' style='margin-bottom: 20px;'>";
    ss_buffer << "<label for='num_bands'>Number of bands:</label>";
    ss_buffer << "<input type='number' id='num_bands' name='num_bands' value='" << std::to_string(config.num_bands)
            << "' min='1' max='" << MAX_BANDS << "' onchange='updateBandRows()'>";
    ss_buffer << "</div>";
    // Insert Bands Table here
    ss_buffer << "<table>";
    ss_buffer << "<thead style='background-color: var(--primary-color); color: white;'>";
    ss_buffer << "<tr>";
    ss_buffer << "<th>Band</th>";
    ss_buffer << "<th>Start Freq</th>";
    ss_buffer << "<th>End Freq</th>";
    ss_buffer << "<th id='antenna_ports_a_header'>Antenna Ports (Radio A)</th>";
    ss_buffer << "<th id='antenna_ports_b_header'>Antenna Ports (Radio B)</th>";
    ss_buffer << "</tr>";
    ss_buffer << "</thead>";
    ss_buffer << "<tbody>";
    // Send H2 for Band Config, num_bands div, and table header/tbody opening together.
    ret = send_ss_chunk(ss_buffer); 
    if (ret != ESP_OK) return ret;

    for (int i = 0; i < config.num_bands; i++) {
        ss_buffer << "<tr>";
        ss_buffer << "<td><select name='band_" << i << "' onchange='updateFrequencies(this, " << i << ")'>";

        // Find matching band from description
        std::string selected_band;
        for (const auto &[band_name_key, band_val]: HtmlContent::band_info) { // Renamed band_info to band_val to avoid conflict
            if (strcmp(config.bands[0][i].description, band_val.name) == 0) {
                selected_band = band_name_key;
                break;
            }
        }

        // Generate options with correct selection, sorted by frequency (high to low)
        auto sorted_bands = HtmlContent::get_bands_by_frequency();
        for (const auto &[band_name_key, band_val]: sorted_bands) {
            ss_buffer << "<option value='" << band_name_key << "' "
               << (band_name_key == selected_band ? "selected" : "")
               << ">" << band_val.name << "</option>";
        }

        ss_buffer << "</select></td>";
        ss_buffer << "<td>" << config.bands[0][i].start_freq << "</td>"; // Frequencies assumed same for Radio A and B for a given band row
        ss_buffer << "<td>" << config.bands[0][i].end_freq << "</td>";
            
        // Antenna Ports for Radio A
        ss_buffer << "<td>";
        for (int j = 0; j < config.num_antenna_ports; j++) {
            ss_buffer << "<input type='checkbox' name='ports_a_" << i << "_" << j << "' value='1' "
                    << (config.bands[0][i].antenna_ports[j] ? "checked" : "") << ">" << (j + 1) << " ";
        }
        ss_buffer << "</td>";

        // Antenna Ports for Radio B
        ss_buffer << "<td class='radio_b_ports_cell'>"; // Added class for easier JS targeting if needed
        for (int j = 0; j < config.num_antenna_ports; j++) {
            bool radio_b_port_checked = config.bands[1][i].antenna_ports[j];
            ss_buffer << "<input type='checkbox' name='ports_b_" << i << "_" << j << "' value='1' "
                    << (radio_b_port_checked ? "checked" : "") << ">" << (j + 1) << " ";
        }
        ss_buffer << "</td></tr>";
        ret = send_ss_chunk(ss_buffer); // Send each row as a chunk
        if (ret != ESP_OK) return ret;
    }

    ss_buffer << "</tbody>";
    ss_buffer << "</table>";
    // Send table closing tags.
    ret = send_ss_chunk(ss_buffer);
    if (ret != ESP_OK) return ret;

    // The "Configure Bands for Radio:" dropdown was removed as both Radio A and B 
    // configurations are now displayed and editable simultaneously in the table.
    // Original UART block removed.

    // PTT Configuration Section
    ss_buffer << "<h2>PTT Configuration</h2>";

    // PTT Configuration (Radio A)
    ss_buffer << "<h3>Radio A</h3>";
    ss_buffer << "<div class='form-group'>";
    ss_buffer << "<label for='ptt_input_radio_a'>PTT Input Pin (Radio A):</label>";
    ss_buffer << "<select id='ptt_input_radio_a' name='ptt_input_radio_a'>";
    ss_buffer << "<option value='-1' " << (config.ptt_input_radio_a == -1 ? "selected" : "") << ">Disabled</option>";
    for (int pin = 0; pin <= 15; pin++) { // KC868-A16 has 16 inputs (0-15), corresponding to X1-X16
        ss_buffer << "<option value='" << pin << "' "
                  << (config.ptt_input_radio_a == pin ? "selected" : "")
                  << ">X" << (pin + 1 < 10 ? "0" : "") << (pin + 1) << "</option>";
    }
    ss_buffer << "</select></div>";

    ss_buffer << "<div class='form-group'>";
    ss_buffer << "<label for='ptt_input_radio_a_active_high'>PTT Active Level (Radio A):</label>";
    ss_buffer << "<select id='ptt_input_radio_a_active_high' name='ptt_input_radio_a_active_high'>";
    ss_buffer << "<option value='true' " << (config.ptt_input_radio_a_active_high ? "selected" : "") << ">Active High</option>";
    ss_buffer << "<option value='false' " << (!config.ptt_input_radio_a_active_high ? "selected" : "") << ">Active Low</option>";
    ss_buffer << "</select></div>";

    // Radio B PTT Configuration (conditionally visible)
    ss_buffer << "<div id='ptt_config_radio_b_div' style='display: "
              << (config.radio_operation_mode != RADIO_OP_MODE_SINGLE_A ? "block" : "none") << ";'>";
    ss_buffer << "<h3>Radio B</h3>"; // Changed from H2 to H3
    ss_buffer << "<div class='form-group'>";
    ss_buffer << "<label for='ptt_input_radio_b'>PTT Input Pin (Radio B):</label>";
    ss_buffer << "<select id='ptt_input_radio_b' name='ptt_input_radio_b'>";
    ss_buffer << "<option value='-1' " << (config.ptt_input_radio_b == -1 ? "selected" : "") << ">Disabled</option>";
    for (int pin = 0; pin <= 15; pin++) { // KC868-A16 has 16 inputs (0-15), corresponding to X1-X16
        ss_buffer << "<option value='" << pin << "' "
                  << (config.ptt_input_radio_b == pin ? "selected" : "")
                  << ">X" << (pin + 1 < 10 ? "0" : "") << (pin + 1) << "</option>";
    }
    ss_buffer << "</select></div>";

    ss_buffer << "<div class='form-group'>";
    ss_buffer << "<label for='ptt_input_radio_b_active_high'>PTT Active Level (Radio B):</label>";
    ss_buffer << "<select id='ptt_input_radio_b_active_high' name='ptt_input_radio_b_active_high'>";
    ss_buffer << "<option value='true' " << (config.ptt_input_radio_b_active_high ? "selected" : "") << ">Active High</option>";
    ss_buffer << "<option value='false' " << (!config.ptt_input_radio_b_active_high ? "selected" : "") << ">Active Low</option>";
    ss_buffer << "</select></div>";
    ss_buffer << "</div>"; // End of ptt_config_radio_b_div
    
    ret = send_ss_chunk(ss_buffer); // Send the entire PTT configuration section
    if (ret != ESP_OK) return ret;

    // Relay Names Section
    ss_buffer << "<div class='relay-names-container' style='background-color: var(--card-background-color); border-radius: 10px; padding: 20px; box-shadow: 0 4px 6px rgba(0,0,0,0.1); margin-bottom: 30px;'>";
    ss_buffer << "<h2>Relay Names</h2>";
    ss_buffer << "<p>Configure names for each relay:</p>";
    
    ss_buffer << "<table class='relay-names-table' style='width: 100%; border-collapse: collapse; margin-top: 15px;'>";
    ss_buffer << "<thead><tr><th>Relay</th><th>Custom Name</th></tr></thead>";
    ss_buffer << "<tbody>";
    
    // First 8 relays (Radio A)
    for (int i = 0; i < 8; i++) {
        // Use default name if custom name is empty
        std::string display_name;
        if (config.relay_names[i][0] != '\0') {
            display_name = config.relay_names[i];
        } else {
            display_name = "Relay " + std::to_string(i + 1);
        }
        
        ss_buffer << "<tr>";
        ss_buffer << "<td>Relay " << (i + 1) << " (Radio A)</td>";
        ss_buffer << "<td><input type='text' name='relay_name_" << i << "' value='" 
                  << display_name << "' maxlength='31' onchange='mirrorRelayName(" << i << ", " << (i + 8) << ")'></td>";
        ss_buffer << "</tr>";
    }
    
    // We don't need to show Radio B relay names in the configuration page
    // They are automatically mirrored from Radio A
    
    ss_buffer << "</tbody></table>";
    ss_buffer << "</div>"; // End of relay-names-container

    ss_buffer << "<h2>Data Source Configuration</h2>";

    // Moved "Allow concurrent data sources" checkbox here
    ss_buffer << "<div class='auto-mode-container'>"; // Re-using auto-mode-container class for styling
    ss_buffer << "<label>";
    ss_buffer << "<input type='checkbox' name='allow_concurrent_data_sources' " << (config.allow_concurrent_data_sources ? "checked" : "") << ">";
    ss_buffer << " Allow concurrent UART and MQTT data sources";
    ss_buffer << "</label>";
    ss_buffer << "</div>";

    // UART Configuration
    ss_buffer << "<h3>CAT Data (UART)</h3>";
    ss_buffer << "<div class='form-group' style='margin-bottom: 20px;'>";
    ss_buffer << "<label for='uart_baud_rate'>Baud Rate:</label>";
    ss_buffer << "<select id='uart_baud_rate' name='uart_baud_rate'>";
    for (const int baud_rates[] = {1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200}; const int rate: baud_rates) {
        ss_buffer << "<option value='" << rate << "' "
                << (config.uart_baud_rate == rate ? "selected" : "")
                << ">" << rate << "</option>";
    }
    ss_buffer << "</select></div>";
    ss_buffer << "<div class='form-group'>";
    ss_buffer << "<label for='uart_parity'>Parity:</label>";
    ss_buffer << "<select id='uart_parity' name='uart_parity'>";
    ss_buffer << "<option value='0' " << (config.uart_parity == 0 ? "selected" : "") << ">None</option>";
    ss_buffer << "<option value='2' " << (config.uart_parity == 2 ? "selected" : "") << ">Even</option>";
    ss_buffer << "<option value='3' " << (config.uart_parity == 3 ? "selected" : "") << ">Odd</option>";
    ss_buffer << "</select></div>";
    ss_buffer << "<div class='form-group'>";
    ss_buffer << "<label for='uart_stop_bits'>Stop Bits:</label>";
    ss_buffer << "<select id='uart_stop_bits' name='uart_stop_bits'>";
    ss_buffer << "<option value='1' " << (config.uart_stop_bits == 1 ? "selected" : "") << ">1</option>";
    ss_buffer << "<option value='2' " << (config.uart_stop_bits == 2 ? "selected" : "") << ">1.5</option>";
    ss_buffer << "<option value='3' " << (config.uart_stop_bits == 3 ? "selected" : "") << ">2</option>";
    ss_buffer << "</select></div>";
    ss_buffer << "<div class='form-group'>";
    ss_buffer << "<label for='uart_flow_ctrl'>Flow Control:</label>";
    ss_buffer << "<select id='uart_flow_ctrl' name='uart_flow_ctrl'>";
    ss_buffer << "<option value='0' " << (config.uart_flow_ctrl == 0 ? "selected" : "") << ">None</option>";
    ss_buffer << "<option value='1' " << (config.uart_flow_ctrl == 1 ? "selected" : "") << ">RTS</option>";
    ss_buffer << "<option value='2' " << (config.uart_flow_ctrl == 2 ? "selected" : "") << ">CTS</option>";
    ss_buffer << "<option value='3' " << (config.uart_flow_ctrl == 3 ? "selected" : "") << ">CTS/RTS</option>";
    ss_buffer << "</select></div>";
    ss_buffer << "<div class='form-group'>";
    ss_buffer << "<label for='uart_tx_pin'>UART TX Pin:</label>";
    ss_buffer << "<select id='uart_tx_pin' name='uart_tx_pin'>";
    ss_buffer << "<option value='-1' " << (config.uart_tx_pin == -1 ? "selected" : "") << ">Disabled</option>";
    for (int pin = 0; pin <= 39; pin++) {
        if (pin == -1) continue; 
        ss_buffer << "<option value='" << pin << "' "
           << (config.uart_tx_pin == pin ? "selected" : "")
           << ">GPIO" << pin << "</option>";
    }
    ss_buffer << "</select></div>";
    ss_buffer << "<div class='form-group'>";
    ss_buffer << "<label for='uart_rx_pin'>UART RX Pin:</label>";
    ss_buffer << "<select id='uart_rx_pin' name='uart_rx_pin'>";
    for (int pin = 0; pin <= 39; pin++) {
        ss_buffer << "<option value='" << pin << "' "
           << (config.uart_rx_pin == pin ? "selected" : "")
           << ">GPIO" << pin << "</option>";
    }
    ss_buffer << "</select></div>";
    ret = send_ss_chunk(ss_buffer); // Chunk for UART settings
    if (ret != ESP_OK) return ret;

    // MQTT Configuration
    ss_buffer << "<h3>MQTT</h3>";
    ss_buffer << "<div class='form-group'>";
    ss_buffer << "<label>";
    ss_buffer << "<input type='checkbox' name='mqtt_enabled' " << (config.mqtt_enabled ? "checked" : "") << ">";
    ss_buffer << " Enable MQTT";
    ss_buffer << "</label>";
    ss_buffer << "</div>";
    ss_buffer << "<div class='form-group'>";
    ss_buffer << "<label for='mqtt_broker'>MQTT Broker:</label>";
    ss_buffer << "<input type='text' id='mqtt_broker' name='mqtt_broker' value='" << config.mqtt_broker << "'>";
    ss_buffer << "</div>";
    ss_buffer << "<div class='form-group'>";
    ss_buffer << "<label for='mqtt_port'>MQTT Port:</label>";
    ss_buffer << "<input type='number' id='mqtt_port' name='mqtt_port' value='" << config.mqtt_port << "'>";
    ss_buffer << "</div>";
    ss_buffer << "<div class='form-group'>";
    ss_buffer << "<label for='mqtt_rig_id'>Rig ID:</label>";
    ss_buffer << "<input type='text' id='mqtt_rig_id' name='mqtt_rig_id' value='" << config.mqtt_rig_id << "'>";
    ss_buffer << "</div>";
    ss_buffer << "<div class='form-group'>";
    ss_buffer << "<label for='mqtt_username'>MQTT Username:</label>";
    ss_buffer << "<input type='text' id='mqtt_username' name='mqtt_username' value='" << (config.mqtt_username[0] != '\0' ? config.mqtt_username : "") << "'>";
    ss_buffer << "</div>";
    ss_buffer << "<div class='form-group'>";
    ss_buffer << "<label for='mqtt_password'>MQTT Password:</label>";
    ss_buffer << "<input type='password' id='mqtt_password' name='mqtt_password' value='" << (config.mqtt_password[0] != '\0' ? config.mqtt_password : "") << "'>";
    ss_buffer << "</div>";
    ss_buffer << "<div class='form-group'>";
    ss_buffer << "<label for='mqtt_client_id'>MQTT Client ID:</label>";
    ss_buffer << "<input type='text' id='mqtt_client_id' name='mqtt_client_id' value='" << config.mqtt_client_id << "'>";
    ss_buffer << "</div>";
    ss_buffer << "<div class='form-group'>";
    ss_buffer << "<label for='mqtt_topic'>MQTT Topic:</label>";
    ss_buffer << "<input type='text' id='mqtt_topic' name='mqtt_topic' value='" << config.mqtt_topic << "'>";
    ss_buffer << "</div>";
    ret = send_ss_chunk(ss_buffer);
    if (ret != ESP_OK) return ret;

    // WebSocket Configuration
    ss_buffer << "<h3>WebSocket</h3>";
    ss_buffer << "<div class='form-group'>";
    ss_buffer << "<label>";
    ss_buffer << "<input type='checkbox' name='websocket_enabled' " << (config.websocket_enabled ? "checked" : "") << ">";
    ss_buffer << " Enable WebSocket Server";
    ss_buffer << "</label>";
    ss_buffer << "<div class='form-description' style='margin-left: 20px; font-size: 0.9em; color: #666;'>";
    ss_buffer << "Enables real-time WebSocket API at ws://device-ip/ws for status updates and control.";
    ss_buffer << "</div>";
    ss_buffer << "</div>";
    ret = send_ss_chunk(ss_buffer);
    if (ret != ESP_OK) return ret;

    ss_buffer << "<div class='button-container' style='margin: 20px 0;'>";
    ss_buffer << "<input type='submit' value='Update Configuration' class='button'>";
    ss_buffer << "</div>";
    ss_buffer << "</form>";
    ret = send_ss_chunk(ss_buffer);
    if (ret != ESP_OK) return ret;

    // Send JavaScript in manageable parts.
    ss_buffer << R"(
    <script>
    // Theme handling
    function setTheme(theme) {
        document.documentElement.setAttribute('data-theme', theme);
        localStorage.setItem('theme', theme);
    }

    function toggleTheme() {
        const currentTheme = document.documentElement.getAttribute('data-theme') || 'light';
        const newTheme = currentTheme === 'light' ? 'dark' : 'light';
        setTheme(newTheme);
    }

    // Initialize theme
    const savedTheme = localStorage.getItem('theme') || 
                      (window.matchMedia('(prefers-color-scheme: dark)').matches ? 'dark' : 'light');
    setTheme(savedTheme);

    async function submitConfig(event) {
        event.preventDefault();
        const form = document.getElementById('configForm');
        const formData = new FormData(form);
        
        // Convert form data to JSON structure
        const config = {
            auto_mode: formData.get('auto_mode') === 'on',
            ai_mode: formData.get('ai_mode') === 'on',
            rx_antenna_enabled: formData.get('rx_antenna_enabled') === 'on',
            allow_concurrent_data_sources: formData.get('allow_concurrent_data_sources') === 'on',
            radio_operation_mode: formData.get('radio_operation_mode'),
            interlock_auto_resolves_conflict: formData.get('interlock_auto_resolves_conflict') === 'on',
            auto_restore_on_conflict_resolution: formData.get('auto_restore_on_conflict_resolution') === 'on',
            radio_restore_delay_ms: parseInt(formData.get('radio_restore_delay_ms')) || 200, // Added this line
            num_bands: parseInt(formData.get('num_bands')),
            num_antenna_ports: parseInt(formData.get('num_antenna_ports')),
            uart_baud_rate: parseInt(formData.get('uart_baud_rate')) || 9600,
            uart_parity: parseInt(formData.get('uart_parity')) || 0,
            uart_stop_bits: parseInt(formData.get('uart_stop_bits')) || 1,
            uart_flow_ctrl: parseInt(formData.get('uart_flow_ctrl')) || 0,
            uart_tx_pin: parseInt(formData.get('uart_tx_pin')) || 17,
            uart_rx_pin: parseInt(formData.get('uart_rx_pin')) || 16,
            
            ptt_input_radio_a: parseInt(formData.get('ptt_input_radio_a')),
            ptt_input_radio_a_active_high: formData.get('ptt_input_radio_a_active_high') === 'true',
            ptt_input_radio_b: parseInt(formData.get('ptt_input_radio_b')), 
            ptt_input_radio_b_active_high: formData.get('ptt_input_radio_b_active_high') === 'true',

            mqtt_enabled: formData.get('mqtt_enabled') === 'on',
            websocket_enabled: formData.get('websocket_enabled') === 'on',
            mqtt_broker: formData.get('mqtt_broker'),
            mqtt_port: parseInt(formData.get('mqtt_port')),
            mqtt_rig_id: formData.get('mqtt_rig_id'),
            mqtt_username: formData.get('mqtt_username') || '',
            mqtt_password: formData.get('mqtt_password') || '',
            mqtt_client_id: formData.get('mqtt_client_id') || 'core-mosquitto',
            mqtt_topic: formData.get('mqtt_topic') || 'omnirig/frequent/radio_info',
            bands: [],
            relay_names: []
        };
        
        // Process relay names
        for (let i = 0; i < 8; i++) {
            // Get Radio A relay name, use default if empty
            let name = formData.get(`relay_name_${i}`);
            if (!name || name.trim() === '') {
                name = `Relay ${i+1}`;
            }
            config.relay_names[i] = name;
            
            // Mirror to Radio B (i+8)
            config.relay_names[i+8] = name;
        }
        
        // Process bands
        for (let i = 0; i < config.num_bands; i++) {
            const band = {
                description: formData.get(`band_${i}`),
                antenna_ports_a: [],
                antenna_ports_b: []
            };
            
            for (let j = 0; j < config.num_antenna_ports; j++) {
                band.antenna_ports_a[j] = formData.get(`ports_a_${i}_${j}`) === '1';
            }

            for (let j = 0; j < config.num_antenna_ports; j++) {
                band.antenna_ports_b[j] = formData.get(`ports_b_${i}_${j}`) === '1';
            }
            config.bands.push(band);
        }
        
        try {
            const response = await fetch('/config', {
                method: 'POST',
                headers: {
                    'Content-Type': 'application/json'
                },
                body: JSON.stringify(config)
            });
            
            if (response.ok) {
                window.location.href = '/';
            } else {
                alert('Failed to update configuration');
            }
        } catch (error) {
            console.error('Error:', error);
            alert('Failed to update configuration');
        }
    }

    const bandFrequencies = {
    )"; // End of the first major script R-string, includes start of bandFrequencies
    ret = send_ss_chunk(ss_buffer); // Send the first part of JS including "const bandFrequencies = {"
    if (ret != ESP_OK) return ret;

// Add the band frequencies mapping
// ss_buffer is now empty due to send_ss_chunk
for (const auto &[fst, snd] : HtmlContent::band_info) {
    // Properties are sent in the next chunk
    ss_buffer << "    '" << fst << "': {start: " << snd.start_freq 
               << ", end: " << snd.end_freq << "},\n";
}
    ret = send_ss_chunk(ss_buffer); // Send the properties chunk
    if (ret != ESP_OK) return ret;

    // ss_buffer is now empty. The next chunk will close the object and add more functions.
    ss_buffer << R"(// -- Properties sent, now close bandFrequencies object and define remaining JS --
    };

    // Create a frequency-ordered array of band keys (highest to lowest frequency)
    const bandsByFrequency = Object.entries(bandFrequencies)
        .sort(([,a], [,b]) => b.start - a.start)
        .map(([key,]) => key);

    function updateFrequencies(selectElement, rowIndex) {
        const selectedBand = selectElement.value;
        const frequencies = bandFrequencies[selectedBand];
        const row = selectElement.closest('tr');
        const cells = row.cells;
        
        cells[1].textContent = frequencies.start;
        cells[2].textContent = frequencies.end;
    }

    function updateAntennaPorts() {
        const numPorts = parseInt(document.getElementById('num_antenna_ports').value);
        const rows = document.querySelectorAll('tbody tr');
        
        rows.forEach((row) => {
            const bandSelect = row.querySelector('select[name^="band_"]');
            const bandIndex = bandSelect.name.split('_')[1];

            const portCellA = row.cells[3]; 
            const existingStatesA = Array.from(portCellA.querySelectorAll('input[type="checkbox"]'))
                .map(cb => cb.checked);
            let portsHtmlA = '';
            for (let j = 0; j < numPorts; j++) {
                const isChecked = existingStatesA[j] ? 'checked' : '';
                portsHtmlA += `<input type="checkbox" name="ports_a_${bandIndex}_${j}" value="1" ${isChecked}>${j + 1} `;
            }
            portCellA.innerHTML = portsHtmlA;

            const portCellB = row.cells[4]; 
            if (portCellB) { 
                const existingStatesB = Array.from(portCellB.querySelectorAll('input[type="checkbox"]'))
                    .map(cb => cb.checked);
                let portsHtmlB = '';
                for (let j = 0; j < numPorts; j++) {
                    const isChecked = existingStatesB[j] ? 'checked' : '';
                    portsHtmlB += `<input type="checkbox" name="ports_b_${bandIndex}_${j}" value="1" ${isChecked}>${j + 1} `;
                }
                portCellB.innerHTML = portsHtmlB;
            }
        });
    }

    function debounce(func, wait) {
        let timeout;
        return function executedFunction(...args) {
            const later = () => {
                clearTimeout(timeout);
                func(...args);
            };
            clearTimeout(timeout);
            timeout = setTimeout(later, wait);
        };
    }

    function updateBandRows() {
        const numBands = parseInt(document.getElementById('num_bands').value);
        const numPorts = parseInt(document.getElementById('num_antenna_ports').value);
        const tbody = document.querySelector('tbody');
        
        const existingConfig = [];
        const existingRows = tbody.querySelectorAll('tr');
        existingRows.forEach((row, i) => {
            const bandSelect = row.querySelector(`select[name="band_${i}"]`);
            const checkboxesA = row.cells[3].querySelectorAll('input[type="checkbox"]');
            const checkboxesB = row.cells[4].querySelectorAll('input[type="checkbox"]');
            existingConfig[i] = {
                band: bandSelect ? bandSelect.value : null,
                ports_a: Array.from(checkboxesA).map(cb => cb.checked),
                ports_b: Array.from(checkboxesB).map(cb => cb.checked)
            };
        });
        
        const fragment = document.createDocumentFragment();
        
        for (let i = 0; i < numBands; i++) {
            const row = document.createElement('tr');
            
            const bandCell = document.createElement('td');
            const bandSelect = document.createElement('select');
            bandSelect.name = `band_${i}`;
            bandSelect.setAttribute('onchange', `updateFrequencies(this, ${i})`);
            
            let optionsHtml = '';
            Object.entries(bandFrequencies).forEach(([band, freq]) => {
                optionsHtml += `<option value="${band}">${band}</option>`;
            });
            bandSelect.innerHTML = optionsHtml;

            if (existingConfig[i] && existingConfig[i].band) {
                 bandSelect.value = existingConfig[i].band;
            } else {
                const defaultBandKey = bandsByFrequency[i % bandsByFrequency.length];
                if (defaultBandKey) bandSelect.value = defaultBandKey;
            }
            
            bandCell.appendChild(bandSelect);
            
            const startFreqCell = document.createElement('td');
            const endFreqCell = document.createElement('td');
            const selectedBandData = bandFrequencies[bandSelect.value];
            if (selectedBandData) {
                startFreqCell.textContent = selectedBandData.start;
                endFreqCell.textContent = selectedBandData.end;
            }
            
            const portsCellA = document.createElement('td');
            let portsHtmlA = '';
            for (let j = 0; j < numPorts; j++) {
                const isCheckedA = existingConfig[i] && existingConfig[i].ports_a && existingConfig[i].ports_a[j] ? 'checked' : '';
                portsHtmlA += `<input type="checkbox" name="ports_a_${i}_${j}" value="1" ${isCheckedA}>${j + 1} `;
            }
            portsCellA.innerHTML = portsHtmlA;

            const portsCellB = document.createElement('td');
            portsCellB.classList.add('radio_b_ports_cell');
            let portsHtmlB = '';
            for (let j = 0; j < numPorts; j++) {
                const isCheckedB = existingConfig[i] && existingConfig[i].ports_b && existingConfig[i].ports_b[j] ? 'checked' : '';
                portsHtmlB += `<input type="checkbox" name="ports_b_${i}_${j}" value="1" ${isCheckedB}>${j + 1} `;
            }
            portsCellB.innerHTML = portsHtmlB;
            
            row.appendChild(bandCell);
            row.appendChild(startFreqCell);
            row.appendChild(endFreqCell);
            row.appendChild(portsCellA);
            row.appendChild(portsCellB);
            fragment.appendChild(row);
        }
        
        tbody.innerHTML = '';
        tbody.appendChild(fragment);
        toggleRadioBPortVisibility(); // Ensure visibility is correct after rebuilding rows
    }

    document.getElementById('num_antenna_ports').addEventListener('change', debounce(updateAntennaPorts, 250));
    document.getElementById('num_bands').addEventListener('change', debounce(updateBandRows, 250));

    function mirrorRelayName(sourceIndex, targetIndex) {
        // When a Radio A relay name is changed, update the corresponding Radio B relay name
        // in the form data that will be submitted (even though we don't show the B inputs)
        const sourceInput = document.querySelector(`input[name="relay_name_${sourceIndex}"]`);
        if (sourceInput) {
            // Create a hidden input for the Radio B relay if it doesn't exist
            let targetInput = document.querySelector(`input[name="relay_name_${targetIndex}"]`);
            if (!targetInput) {
                targetInput = document.createElement('input');
                targetInput.type = 'hidden';
                targetInput.name = `relay_name_${targetIndex}`;
                document.getElementById('configForm').appendChild(targetInput);
            }
            targetInput.value = sourceInput.value;
        }
    }

    function toggleRadioBPortVisibility() {
        const mode = document.getElementById('radio_operation_mode').value;
        const radioBPortHeader = document.getElementById('antenna_ports_b_header');
        const radioBPortCells = document.querySelectorAll('.radio_b_ports_cell');
        const radioBRelayRows = document.querySelectorAll('.radio_b_relay_row');
        const pttConfigRadioBDiv = document.getElementById('ptt_config_radio_b_div');

        const showRadioB = mode !== 'SINGLE_A';
        
        if (radioBPortHeader) {
            radioBPortHeader.style.display = showRadioB ? '' : 'none';
        }
        
        radioBPortCells.forEach(cell => {
            cell.style.display = showRadioB ? '' : 'none';
        });
        radioBRelayRows.forEach(row => {
            row.style.display = showRadioB ? '' : 'none';
        });

        if (pttConfigRadioBDiv) {
            pttConfigRadioBDiv.style.display = showRadioB ? '' : 'none';
        }
 
        const interlockDiv = document.getElementById('interlock_options_div');
        const autoRestoreDiv = document.getElementById('auto_restore_option_div');
        const interlockRestoreDelayDiv = document.getElementById('interlock_restore_delay_div'); // Get the new div
        const interlockCheckbox = document.querySelector('input[name="interlock_auto_resolves_conflict"]');
        const autoRestoreCheckbox = document.querySelector('input[name="auto_restore_on_conflict_resolution"]');


        if (interlockDiv && autoRestoreDiv && interlockRestoreDelayDiv && interlockCheckbox && autoRestoreCheckbox) { // Check all elements
            if (mode === 'CONCURRENT_AB') {
                interlockDiv.style.display = 'block';
                const showAutoRestore = interlockCheckbox.checked;
                autoRestoreDiv.style.display = showAutoRestore ? 'block' : 'none';
                // Show delay input if auto-restore is shown AND checked
                interlockRestoreDelayDiv.style.display = (showAutoRestore && autoRestoreCheckbox.checked) ? 'block' : 'none';
            } else {
                interlockDiv.style.display = 'none';
                autoRestoreDiv.style.display = 'none';
                interlockRestoreDelayDiv.style.display = 'none'; // Hide delay if interlock section is hidden
            }
        }
    }
    document.getElementById('radio_operation_mode').addEventListener('change', toggleRadioBPortVisibility);
    document.querySelector('input[name="interlock_auto_resolves_conflict"]').addEventListener('change', toggleRadioBPortVisibility);
    document.querySelector('input[name="auto_restore_on_conflict_resolution"]').addEventListener('change', toggleRadioBPortVisibility); // Add listener
    
    toggleRadioBPortVisibility(); // Initial call

    // Handle configuration file import
    async function handleConfigImport(input) {
        if (!input.files || input.files.length === 0) {
            return;
        }

        const file = input.files[0];
        if (!file.name.toLowerCase().endsWith('.json')) {
            alert('Please select a valid JSON configuration file.');
            return;
        }

        if (!confirm('Are you sure you want to import this configuration? This will overwrite your current settings.')) {
            input.value = ''; // Clear the file input
            return;
        }

        try {
            const formData = new FormData();
            formData.append('config', file);

            const submitButton = document.querySelector('input[type="submit"]');
            const originalValue = submitButton.value;
            submitButton.value = 'Importing...';
            submitButton.disabled = true;

            const response = await fetch('/api/config/import', {
                method: 'POST',
                body: file
            });

            const result = await response.json();
            
            if (response.ok && result.status === 'success') {
                alert('Configuration imported successfully! The page will reload to show the new settings.');
                window.location.reload();
            } else {
                const errorMessage = result.message || 'Failed to import configuration';
                const errorCode = result.error_code || '';
                alert(`Error: ${errorMessage}${errorCode ? ' (' + errorCode + ')' : ''}`);
            }
        } catch (error) {
            console.error('Error importing configuration:', error);
            alert('Failed to import configuration. Please check the file format and try again.');
        } finally {
            // Reset the submit button
            const submitButton = document.querySelector('input[type="submit"]');
            if (submitButton) {
                submitButton.value = originalValue || 'Update Configuration';
                submitButton.disabled = false;
            }
            input.value = ''; // Clear the file input
        }
    }

    // Initialize frequencies on page load
    function initializeFrequencies() {
        const bandSelects = document.querySelectorAll('select[name^="band_"]');
        bandSelects.forEach((select, index) => {
            updateFrequencies(select, index);
        });
    }

    // Call initialization when DOM is loaded
    document.addEventListener('DOMContentLoaded', initializeFrequencies);
    </script>)";
    ret = send_ss_chunk(ss_buffer);
    if (ret != ESP_OK) return ret;

    ss_buffer << "<div class='button-container' style='margin: 20px 0;'>";
    ss_buffer << "<a href='/' class='button'>Back to Home</a>";
    
    // Export configuration button
    ss_buffer << "<a href='/api/config/export' download='kc868_config.json' class='button' style='background-color: #3498db; color: white; margin-left: 10px;'>Export Configuration</a>";
    
    // Import configuration section
    ss_buffer << "<div style='display: inline-block; margin-left: 10px;'>";
    ss_buffer << "<input type='file' id='configFileInput' accept='.json' style='display: none;' onchange='handleConfigImport(this)'>";
    ss_buffer << "<button onclick='document.getElementById(\"configFileInput\").click()' class='button' style='background-color: #2ecc71; color: white;'>Import Configuration</button>";
    ss_buffer << "</div>";
    
    // Reset configuration button
    ss_buffer << "<form action='/reset-config' method='post' style='display: inline; margin-left: 10px;'>";
    ss_buffer << "<input type='submit' value='Reset Configuration' class='button' style='background-color: #e74c3c; color: white;' onclick='return confirm(\"Are you sure you want to reset the configuration?\");'>";
    ss_buffer << "</form>";
    ss_buffer << "</div>";
    ret = send_ss_chunk(ss_buffer);
    if (ret != ESP_OK) return ret;

    ret = send_cstr_chunk(HtmlContent::HTML_FOOTER);
    if (ret != ESP_OK) return ret;

    // Send final empty chunk to terminate the response
    return httpd_resp_send_chunk(req, nullptr, 0);
}
