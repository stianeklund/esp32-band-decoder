#include "html_content.h"

#include <cstring>
#include <sstream>
#include <esp_log.h>

auto TAG = "HTML";

// ReSharper disable once CppUseAuto
const char *HTML_HEADER = R"(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no">
    <title>Antenna Switch Controller</title>
    <style>
        :root {
            --primary-color: #3498db;
            --secondary-color: #2c3e50;
            --background-color: #ecf0f1;
            --text-color: #34495e;
            --border-color: #bdc3c7;
        }
        /* Base responsive layout */
        body {
            font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;
            line-height: 1.6;
            color: var(--text-color);
            max-width: 1200px;
            margin: 0 auto;
            padding: 15px;
            background-color: var(--background-color);
        }

        h1, h2 {
            color: var(--secondary-color);
            text-align: center;
            margin-bottom: 30px;
        }

        /* Responsive status container */
        .status-container {
            display: flex;
            flex-direction: column;
            gap: 20px;
            margin-bottom: 30px;
        }

        .status-box {
            background-color: white;
            border-radius: 10px;
            padding: 15px;
            box-shadow: 0 4px 6px rgba(0,0,0,0.1);
            width: 100%;
            transition: transform 0.3s ease;
        }

        .status-box:hover {
            transform: translateY(-5px);
        }

        /* Responsive tables */
        table {
            width: 100%;
            display: table;
            border-collapse: separate;
            border-spacing: 0;
            margin-bottom: 20px;
            border-radius: 10px;
            overflow-x: auto;
        }

        th, td {
            padding: 15px;
            text-align: left;
            border-bottom: 1px solid var(--border-color);
        }

        th {
            font-weight: bold;
            color: white;
            background-color: var(--primary-color);
        }

        tr:last-child td {
            border-bottom: none;
        }

        /* Responsive relay grid */
        .relay-grid {
            display: grid;
            grid-template-columns: repeat(2, 1fr);
            gap: 10px;
        }

        .relay-button {
            padding: 12px 8px;
            border: 1px solid #ddd;
            border-radius: 8px;
            background-color: #e0e0e0;  /* Default state - OFF */
            color: var(--text-color);
            cursor: pointer;
            transition: all 0.3s ease;
            font-weight: bold;
            font-size: 14px;
        }

        .relay-button.active {
            background-color: #2ecc71;  /* Selected but not transmitting - green */
            color: white;
            border-color: #27ae60;
        }

        .relay-button:hover {
            transform: translateY(-2px);
            box-shadow: 0 4px 6px rgba(0,0,0,0.1);
        }

        .relay-button.active.transmitting {
            background-color: #e74c3c;  /* Selected and transmitting - red */
            color: white;
            border-color: #c0392b;
        }

        /* Responsive button container */
        .button-container {
            display: flex;
            flex-wrap: wrap;
            gap: 10px;
            justify-content: center;
        }

        .button, input[type="submit"] {
            width: 100%;
            max-width: 300px;
            margin: 5px 0;
            display: inline-block;
            background-color: var(--primary-color);
            color: white;
            padding: 12px 24px;
            border-radius: 25px;
            text-decoration: none;
            transition: all 0.3s ease;
            border: none;
            cursor: pointer;
            font-size: 16px;
            font-weight: bold;
            text-transform: uppercase;
            letter-spacing: 1px;
        }

        .button:hover, input[type="submit"]:hover {
            background-color: var(--secondary-color);
            transform: translateY(-2px);
            box-shadow: 0 4px 6px rgba(0,0,0,0.1);
        }

        /* Form responsiveness */
        .config-form {
            background-color: white;
            padding: 15px;
            border-radius: 10px;
            box-shadow: 0 4px 6px rgba(0,0,0,0.1);
        }

        .form-group {
            margin-bottom: 15px;
        }

        label {
            display: block;
            margin-bottom: 8px;
            font-weight: bold;
            color: var(--secondary-color);
        }

        input[type="text"], 
        input[type="number"], 
        select {
            width: 100%;
            padding: 10px;
            margin: 5px 0;
            display: inline-block;
            border: 1px solid var(--border-color);
            border-radius: 4px;
            box-sizing: border-box;
            transition: border-color 0.3s ease;
        }

        input[type="text"]:focus, 
        input[type="number"]:focus, 
        select:focus {
            border-color: var(--primary-color);
            outline: none;
        }

        input[type="checkbox"] {
            margin-right: 5px;
        }

        .relay-groups {
            display: flex;
            flex-direction: column;
            gap: 20px;
        }

        .relay-group {
            background-color: #f5f6fa;
            border-radius: 8px;
            padding: 15px;
        }

        .relay-group h3 {
            margin: 0 0 15px 0;
            color: var(--secondary-color);
            text-align: center;
        }

        .auto-mode-container {
            background-color: white;
            border-radius: 10px;
            padding: 20px;
            box-shadow: 0 4px 6px rgba(0,0,0,0.1);
            margin-bottom: 30px;
            transition: all 0.3s ease;
        }

        .auto-mode-container h2 {
            margin-top: 0;
            color: var(--secondary-color);
        }

        .auto-mode-container label {
            display: flex;
            align-items: center;
            font-weight: normal;
            color: var(--text-color);
        }

        .auto-mode-container input[type="checkbox"] {
            margin-right: 10px;
            accent-color: var(--primary-color);
        }

        /* Media Queries for different screen sizes */
        @media (min-width: 768px) {
            body {
                padding: 20px;
            }

            .status-container {
                flex-direction: row;
            }

            .status-box {
                width: 48%;
            }

            .relay-grid {
                grid-template-columns: repeat(4, 1fr);
            }

            .button, input[type="submit"] {
                width: auto;
            }
        }

        /* Configuration page specific */
        @media (max-width: 767px) {
            table th, 
            table td {
                display: block;
                width: 100%;
                box-sizing: border-box;
            }

            table tr {
                margin-bottom: 15px;
                display: block;
            }

            table td {
                border-top: none;
            }

            .config-form table {
                display: block;
                overflow-x: auto;
            }

            .config-form table th,
            .config-form table td {
                min-width: 120px;
            }
        }

        /* Touch-friendly improvements */
        @media (hover: none) {
            .relay-button {
                min-height: 44px; /* Minimum touch target size */
            }

            .button, 
            input[type="submit"],
            select {
                min-height: 44px;
                touch-action: manipulation;
            }

            input[type="checkbox"] {
                min-width: 22px;
                min-height: 22px;
            }
        }

        /* Dark mode support for OLED screens */
        @media (prefers-color-scheme: dark) {
            :root {
                --primary-color: #4a9eff;
                --secondary-color: #a8c7fa;
                --background-color: #121212;
                --text-color: #e0e0e0;
                --border-color: #2d2d2d;
            }

            body {
                background-color: var(--background-color);
            }

            .status-box,
            .relay-group,
            .config-form,
            .auto-mode-container {
                background-color: #1e1e1e;
            }

            input[type="text"],
            input[type="number"],
            select {
                background-color: #2d2d2d;
                color: var(--text-color);
                border-color: var(--border-color);
            }

            .relay-button {
                background-color: #2d2d2d;
                color: var(--text-color);
            }

            .relay-button.active {
                background-color: #2ecc71;
            }
        }
    </style>
</head>
<body>
)";

// ReSharper disable once CppUseAuto
const char *HTML_FOOTER = R"(
</body>
</html>
)";

const std::map<std::string, BandInfo> band_info = {
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

std::string generate_root_html(const antenna_switch_config_t &config, const char *ip_addr, const char *mac_addr) {
    std::stringstream ss;
    ss << HTML_HEADER;
    ss << R"(
    <h1>Antenna Controller</h1>
    <div class="status-container">
        <div class="status-box">
            <h2>Current Status</h2>
            <table>
                <tr><th>Current Frequency</th><td id="current-frequency">Updating...</td></tr>
                <tr><th>Active Relay</th><td id="active-antenna">Updating...</td></tr>
            </table>
        </div>
        <div class="status-box">
            <h2>Network Information</h2>
            <table>
                <tr><th>IP Address</th><td>)" << ip_addr << R"(</td></tr>
                <tr><th>MAC Address</th><td>)" << mac_addr << R"(</td></tr>
            </table>
        </div>
    </div>
    <div class="status-box" style="width: 100%; margin-top: 20px;">
        <h2>Relay Controls</h2>
        <div class="relay-groups">
            <div class="relay-group">
                <h3>Radio A</h3>
                <div class="relay-grid">)";

// First 8 relays (Radio A)
for (int i = 0; i < 8; i++) {
    ss << "<button class='relay-button' data-relay='" << (i + 1) << "' onclick='toggleRelay(" << (i + 1) << ")'>"
       << "Relay " << (i + 1) << "</button>";
}

ss << R"(
                </div>
            </div>
            <div class="relay-group">
                <h3>Radio B</h3>
                <div class="relay-grid">)";

// Next 8 relays (Radio B)
for (int i = 8; i < 16; i++) {
    ss << "<button class='relay-button' data-relay='" << (i + 1) << "' onclick='toggleRelay(" << (i + 1) << ")'>"
       << "Relay " << (i + 1) << "</button>";
}

ss << R"(
                </div>
            </div>
        </div>
    </div>
    <div class="button-container">
        <a href='/config' class="button">Edit Configuration</a>
        <form action='/restart' method='post' style='display:inline' onsubmit='handleRestart(event)'>
            <button type="submit" class="button" style="background-color:#e74c3c">Restart Device</button>
        </form>
        <form action='/reset-wifi' method='post' style='display:inline' onsubmit='return confirm("Reset WiFi credentials?")'>
            <button type="submit" class="button warning">Reset WiFi</button>
        </form>
        <script>
            function handleRestart(event) {
                if (!confirm('Are you sure you want to restart the device?')) {
                    event.preventDefault();
                    return false;
                }
                const button = event.target.querySelector('button');
                button.textContent = 'Restarting...';
                button.disabled = true;

                setTimeout(() => {
                    document.body.innerHTML = '<h1 style="text-align:center;margin-top:50px;">Device is restarting...</h1><p style="text-align:center">This page will refresh in 10 seconds.</p>';
                    setTimeout(() => { window.location.reload(); }, 10000);
                }, 500);

                return true;
            }
        </script>
    </div>
    <script>
        let isRelayOperationInProgress = false;
        const RELAY_OPERATION_COOLDOWN = 250; // ms
        const STATUS_UPDATE_INTERVAL = 2000; // Reduced from 5000 to 2000ms

        async function toggleRelay(relay) {
            // Prevent multiple rapid clicks
            if (isRelayOperationInProgress) {
                //console.log('Operation in progress, please wait...');
                return;
            }

            try {
                isRelayOperationInProgress = true;
                const button = document.querySelector(`button[data-relay="${relay}"]`);
                button.disabled = true; // Disable button during operation
                
                const newState = !button.classList.contains('active');
                //console.log(`Setting relay ${relay} to state: ${newState}`);
                
                const response = await fetch('/relay/control', {
                    method: 'POST',
                    headers: {
                        'Content-Type': 'application/json'
                    },
                    body: JSON.stringify({
                        relay: relay,
                        state: newState
                    })
                });
                
                if (!response.ok) {
                    const errorText = await response.text();
                    console.error('Server error:', errorText);
                    throw new Error(errorText);
                }
                
                const result = await response.json();
                button.classList.toggle('active', result.state);
                
                // Wait for a short period before allowing next operation
                await new Promise(resolve => setTimeout(resolve, RELAY_OPERATION_COOLDOWN));
                
            } catch (error) {
                console.error('Error toggling relay:', error);
                alert('Failed to toggle relay: ' + error.message);
            } finally {
                const button = document.querySelector(`button[data-relay="${relay}"]`);
                button.disabled = false; // Re-enable button
                isRelayOperationInProgress = false;
            }
        }

        // Add debouncing to the status updates
        let statusUpdateTimeout = null;
        async function updateRelayStatus() {
            if (statusUpdateTimeout) {
                clearTimeout(statusUpdateTimeout);
            }
            
            try {
                const response = await fetch('/relay/status');
                const data = await response.json();
                const states = data.states;
                
                for (let i = 1; i <= 16; i++) {
                    const button = document.querySelector(`button[data-relay="${i}"]`);
                    if (button && !button.disabled) { // Only update if button isn't in middle of operation
                        // Subtract 1 from i since relay numbers are 1-based but bits are 0-based
                        // Subtract 1 from i since relay numbers are 1-based but bits are 0-based
                        const state = ((states >> (i-1)) & 1) === 0;  // Inverted logic for active-low relays
                        button.classList.toggle('active', state);
                
                        // Add debug logging
                        //console.debug(`Relay ${i} state: ${state ? 'ON' : 'OFF'}`);
                    }
                }
            } catch (error) {
                console.error('Error updating relay status:', error);
            }
            
            // Schedule next update
            statusUpdateTimeout = setTimeout(updateRelayStatus, STATUS_UPDATE_INTERVAL);
        }

        function updateStatus() {
            fetch("/status")
                .then(response => {
                    if (!response.ok) {
                        throw new Error("Network response was not ok");
                    }
                    return response.json();
                })
                .then(data => {
                    document.getElementById("current-frequency").textContent = data.frequency + " Hz";
                    document.getElementById("active-antenna").textContent = data.antenna;
                    
                    // Get the active antenna number
                    const activeAntennaMatch = data.antenna.match(/Antenna (\d+)/);
                    const activeAntennaNum = activeAntennaMatch ? parseInt(activeAntennaMatch[1]) : null;
                    
                    // Update transmitting state for relay buttons
                    const buttons = document.querySelectorAll('.relay-button');
                    buttons.forEach(button => {
                        const relayNum = parseInt(button.getAttribute('data-relay'));
                        // Only add transmitting class if the button is active (selected)
                        if (button.classList.contains('active')) {
                            if (relayNum === activeAntennaNum && data.transmitting) {
                                button.classList.add('transmitting');
                            } else {
                                button.classList.remove('transmitting');
                            }
                        } else {
                            button.classList.remove('transmitting');
                        }
                    });
                })
                .catch(error => {
                    console.error("Error:", error);
                    document.getElementById("current-frequency").textContent = "Error updating";
                    document.getElementById("active-antenna").textContent = "Error updating";
                });
        }

        // Start the status updates
        updateStatus();
        updateRelayStatus();
        setInterval(updateStatus, STATUS_UPDATE_INTERVAL);
    </script>
    )";
    ss << HTML_FOOTER;
    return ss.str();
}

std::string generate_config_html(const antenna_switch_config_t &config) {
    std::stringstream ss;

    // Check for potential errors before starting to generate HTML
    if (config.num_bands <= 0 || config.num_bands > MAX_BANDS) {
        ESP_LOGE(TAG, "Invalid number of bands: %d (should be between 1 and %d)",
                 config.num_bands, MAX_BANDS);
        return ""; // Return empty string to indicate error
    }
    if (config.num_antenna_ports <= 0 || config.num_antenna_ports > MAX_ANTENNA_PORTS) {
        ESP_LOGE(TAG, "Invalid number of antenna ports: %d (should be between 1 and %d)",
                 config.num_antenna_ports, MAX_ANTENNA_PORTS);
        return ""; // Return empty string to indicate error
    }

    ESP_LOGI(TAG, "Generating HTML for config: %d bands, %d antenna ports", config.num_bands, config.num_antenna_ports);
    ESP_LOGI(TAG, "Debug: num_bands = %d, num_antenna_ports = %d", config.num_bands, config.num_antenna_ports);

    ss << HTML_HEADER;
    ss << "<h2>Relay Configuration</h2>";
    ss << "<form id='configForm' class='config-form' onsubmit='submitConfig(event)'>";
    ss << "<div class='form-group'>";
    ss << "<label for='num_bands'>Number of bands:</label>";
    ss << "<input type='number' id='num_bands' name='num_bands' value='" << std::to_string(config.num_bands)
            << "' min='1' max='" << MAX_BANDS << "' onchange='updateBandRows()'>";
    ss << "</div>";
    ss << "<div class='form-group'>";
    ss << "<h3>Switch Configuration</h3>";
    ss << "<label for='num_antenna_ports'>Number of outputs:</label>";
    ss << "<input type='number' id='num_antenna_ports' name='num_antenna_ports' value='"
            << std::to_string(config.num_antenna_ports) << "' min='1' max='" << MAX_ANTENNA_PORTS << "' onchange='updateAntennaPorts()'>";
    ss << "</div>";

    ss << "<h3>UART Configuration</h3>";
    ss << "<div class='form-group'>";
    ss << "<label for='uart_baud_rate'>Baud Rate:</label>";
    ss << "<select id='uart_baud_rate' name='uart_baud_rate'>";
    for (const int baud_rates[] = {1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200}; const int rate: baud_rates) {
        ss << "<option value='" << rate << "' "
                << (config.uart_baud_rate == rate ? "selected" : "")
                << ">" << rate << "</option>";
    }
    ss << "</select></div>";

    ss << "<div class='form-group'>";
    ss << "<label for='uart_parity'>Parity:</label>";
    ss << "<select id='uart_parity' name='uart_parity'>";
    ss << "<option value='0' " << (config.uart_parity == 0 ? "selected" : "") << ">None</option>";
    ss << "<option value='2' " << (config.uart_parity == 2 ? "selected" : "") << ">Even</option>";
    ss << "<option value='3' " << (config.uart_parity == 3 ? "selected" : "") << ">Odd</option>";
    ss << "</select></div>";

    ss << "<div class='form-group'>";
    ss << "<label for='uart_stop_bits'>Stop Bits:</label>";
    ss << "<select id='uart_stop_bits' name='uart_stop_bits'>";
    ss << "<option value='1' " << (config.uart_stop_bits == 1 ? "selected" : "") << ">1</option>";
    ss << "<option value='2' " << (config.uart_stop_bits == 2 ? "selected" : "") << ">1.5</option>";
    ss << "<option value='3' " << (config.uart_stop_bits == 3 ? "selected" : "") << ">2</option>";
    ss << "</select></div>";

    ss << "<div class='form-group'>";
    ss << "<label for='uart_flow_ctrl'>Flow Control:</label>";
    ss << "<select id='uart_flow_ctrl' name='uart_flow_ctrl'>";
    ss << "<option value='0' " << (config.uart_flow_ctrl == 0 ? "selected" : "") << ">None</option>";
    ss << "<option value='1' " << (config.uart_flow_ctrl == 1 ? "selected" : "") << ">RTS</option>";
    ss << "<option value='2' " << (config.uart_flow_ctrl == 2 ? "selected" : "") << ">CTS</option>";
    ss << "<option value='3' " << (config.uart_flow_ctrl == 3 ? "selected" : "") << ">CTS/RTS</option>";
    ss << "</select></div>";

    ss << "<div class='form-group'>";
    ss << "<label for='uart_tx_pin'>UART TX Pin:</label>";
    ss << "<select id='uart_tx_pin' name='uart_tx_pin'>";
    for (int pin = 0; pin <= 39; pin++) {
        ss << "<option value='" << pin << "' "
           << (config.uart_tx_pin == pin ? "selected" : "")
           << ">GPIO" << pin << "</option>";
    }
    ss << "</select></div>";

    ss << "<div class='form-group'>";
    ss << "<label for='uart_rx_pin'>UART RX Pin:</label>";
    ss << "<select id='uart_rx_pin' name='uart_rx_pin'>";
    for (int pin = 0; pin <= 39; pin++) {
        ss << "<option value='" << pin << "' "
           << (config.uart_rx_pin == pin ? "selected" : "")
           << ">GPIO" << pin << "</option>";
    }
    ss << "</select></div>";

    ss << "<table>";
    ss << "<thead>";
    ss << "<tr>";
    ss << "<th>Band</th>";
    ss << "<th>Start Freq</th>";
    ss << "<th>End Freq</th>";
    ss << "<th>Antenna Ports</th>";
    ss << "</tr>";
    ss << "</thead>";
    ss << "<tbody>";

    for (int i = 0; i < config.num_bands; i++) {
        ss << "<tr>";
        ss << "<td><select name='band_" << i << "' onchange='updateFrequencies(this, " << i << ")'>";

        // Find matching band from description
        std::string selected_band;
        for (const auto &[band_name, band_info]: band_info) {
            if (strcmp(config.bands[i].description, band_info.name) == 0) {
                selected_band = band_name;
                break;
            }
        }

        // Generate options with correct selection
        for (const auto &[band_name, band_info]: band_info) {
            ss << "<option value='" << band_name << "' "
               << (band_name == selected_band ? "selected" : "")
               << ">" << band_info.name << "</option>";
        }

        ss << "</select></td>";
        ss << "<td>" << config.bands[i].start_freq << "</td>";
        ss << "<td>" << config.bands[i].end_freq << "</td>";
        ss << "<td>";

        // Generate checkboxes for each antenna port
        for (int j = 0; j < config.num_antenna_ports; j++) {
            ss << "<input type='checkbox' name='a" << i << "_" << j << "' value='1' "
                    << (config.bands[i].antenna_ports[j] ? "checked" : "") << ">" << (j + 1) << " ";
        }


        ss << "</td></tr>";
    }

    ss << "</tbody>";
    ss << "</table>";

    ss << "<div class='auto-mode-container'>";
    ss << "<h2>Auto Mode</h2>";
    ss << "<label>";
    ss << "<input type='checkbox' name='auto_mode' " << (config.auto_mode ? "checked" : "") << ">";
    ss << " Enable Automatic band selection";
    ss << "</label>";
    ss << "</div>";

    ss << "<div class='button-container'>";
    ss << "<input type='submit' value='Update Configuration'>";
    ss << "</div>";
    ss << "</form>";

    // Add JavaScript for form submission
    ss << R"(
    <script>
    async function submitConfig(event) {
        event.preventDefault();
        const form = document.getElementById('configForm');
        const formData = new FormData(form);
        
        // Convert form data to JSON structure
        const config = {
            auto_mode: formData.get('auto_mode') === 'on',
            num_bands: parseInt(formData.get('num_bands')),
            num_antenna_ports: parseInt(formData.get('num_antenna_ports')),
            tcp_host: formData.get('tcp_host'),
            tcp_port: parseInt(formData.get('tcp_port')),
            uart_baud_rate: parseInt(formData.get('uart_baud_rate')) || 9600,
            uart_parity: parseInt(formData.get('uart_parity')) || 0,
            uart_stop_bits: parseInt(formData.get('uart_stop_bits')) || 1,
            uart_flow_ctrl: parseInt(formData.get('uart_flow_ctrl')) || 0,
            uart_tx_pin: parseInt(formData.get('uart_tx_pin')) || 17,
            uart_rx_pin: parseInt(formData.get('uart_rx_pin')) || 16,
            bands: []
        };
        
        // Process bands
        for (let i = 0; i < config.num_bands; i++) {
            const band = {
                description: formData.get(`band_${i}`),
                antenna_ports: []
            };
            
            // Process antenna ports
            for (let j = 0; j < config.num_antenna_ports; j++) {
                band.antenna_ports[j] = formData.get(`a${i}_${j}`) === '1';
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
    
    // Create a mapping of band frequencies
    const bandFrequencies = {
)";

// Add the band frequencies mapping
for (const auto &band : band_info) {
    ss << "'" << band.first << "': {start: " << band.second.start_freq 
       << ", end: " << band.second.end_freq << "},\n";
}

ss << R"(
    };

    function updateFrequencies(selectElement, rowIndex) {
        const selectedBand = selectElement.value;
        const frequencies = bandFrequencies[selectedBand];
        const row = selectElement.closest('tr');
        const cells = row.cells;
        
        // Update start and end frequency cells
        cells[1].textContent = frequencies.start;
        cells[2].textContent = frequencies.end;
    }

    function updateAntennaPorts() {
        const numPorts = parseInt(document.getElementById('num_antenna_ports').value);
        const rows = document.querySelectorAll('tbody tr');
        
        rows.forEach((row) => {
            const portCell = row.cells[3]; // Antenna ports cell
            const bandSelect = row.querySelector('select[name^="band_"]');
            const bandIndex = bandSelect.name.split('_')[1];
            
            // Store existing checkbox states
            const existingStates = Array.from(portCell.querySelectorAll('input[type="checkbox"]'))
                .map(cb => cb.checked);
            
            let portsHtml = '';
            for (let j = 0; j < numPorts; j++) {
                const isChecked = existingStates[j] ? 'checked' : '';
                portsHtml += `<input type="checkbox" name="a${bandIndex}_${j}" value="1" ${isChecked}>${j + 1} `;
            }
            
            portCell.innerHTML = portsHtml;
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
        
        // Store existing configurations before updating
        const existingConfig = [];
        const existingRows = tbody.querySelectorAll('tr');
        existingRows.forEach((row, i) => {
            const bandSelect = row.querySelector(`select[name="band_${i}"]`);
            const checkboxes = row.querySelectorAll('input[type="checkbox"]');
            existingConfig[i] = {
                band: bandSelect ? bandSelect.value : null,
                ports: Array.from(checkboxes).map(cb => cb.checked)
            };
        });
        
        // Create a document fragment to batch DOM updates
        const fragment = document.createDocumentFragment();
        
        // Generate all rows at once
        for (let i = 0; i < numBands; i++) {
            const row = document.createElement('tr');
            
            // Band selection cell
            const bandCell = document.createElement('td');
            const bandSelect = document.createElement('select');
            bandSelect.name = `band_${i}`;
            bandSelect.setAttribute('onchange', `updateFrequencies(this, ${i})`);
            
            // Create band options HTML string once
            let optionsHtml = '';
            Object.entries(bandFrequencies).forEach(([band, freq]) => {
                optionsHtml += `<option value="${band}">${band}</option>`;
            });
            bandSelect.innerHTML = optionsHtml;
            
            // Set initial band value based on the description in the config
            const bandDescription = document.querySelector(`select[name="band_${i}"]`).value;
            if (bandDescription) {
                bandSelect.value = bandDescription;
            } else {
                // Fallback to default band based on index
                const defaultBand = Object.entries(bandFrequencies)[i % Object.keys(bandFrequencies).length];
                bandSelect.value = defaultBand[0];
            }
            
            bandCell.appendChild(bandSelect);
            
            // Frequency cells
            const startFreqCell = document.createElement('td');
            const endFreqCell = document.createElement('td');
            const selectedBand = bandFrequencies[bandSelect.value];
            startFreqCell.textContent = selectedBand.start;
            endFreqCell.textContent = selectedBand.end;
            
            // Antenna ports cell - create HTML string instead of multiple DOM operations
            const portsCell = document.createElement('td');
            let portsHtml = '';
            for (let j = 0; j < numPorts; j++) {
                const isChecked = existingConfig[i] && existingConfig[i].ports[j] ? 'checked' : '';
                portsHtml += `<input type="checkbox" name="a${i}_${j}" value="1" ${isChecked}>${j + 1} `;
            }
            portsCell.innerHTML = portsHtml;
            
            row.appendChild(bandCell);
            row.appendChild(startFreqCell);
            row.appendChild(endFreqCell);
            row.appendChild(portsCell);
            fragment.appendChild(row);
        }
        
        // Clear and update tbody in one operation
        tbody.innerHTML = '';
        tbody.appendChild(fragment);
    }

    // Add event listeners with debouncing
    document.getElementById('num_antenna_ports').addEventListener('change', debounce(updateAntennaPorts, 250));
    document.getElementById('num_bands').addEventListener('change', debounce(updateBandRows, 250));
    </script>)";
    ss << "<div class='button-container'>";
    ss << "<a href='/' class='button' style='background-color: var(--primary-color);'>Back to Home</a>";
    ss << "<form action='/reset-config' method='post' style='display: inline;'>";
    ss
            << "<input type='submit' value='Reset Configuration' class='button' style='background-color: #e74c3c;' onclick='return confirm(\"Are you sure you want to reset the configuration?\");'>";
    ss << "</form>";
    ss << "</div>";
    ss << HTML_FOOTER;
    return ss.str();
}
