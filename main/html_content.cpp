#include "html_content.h"
#include <sstream>
#include <esp_log.h>

auto TAG = "HTML";

// ReSharper disable once CppUseAuto
const char *HTML_HEADER = R"(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Antenna Switch Controller</title>
    <style>
        :root {
            --primary-color: #3498db;
            --secondary-color: #2c3e50;
            --background-color: #ecf0f1;
            --text-color: #34495e;
            --border-color: #bdc3c7;
        }
        body {
            font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;
            line-height: 1.6;
            color: var(--text-color);
            max-width: 1000px;
            margin: 0 auto;
            padding: 20px;
            background-color: var(--background-color);
        }
        h1, h2 {
            color: var(--secondary-color);
            text-align: center;
            margin-bottom: 30px;
        }
        .status-container {
            display: flex;
            justify-content: space-between;
            margin-bottom: 30px;
        }
        .status-box {
            background-color: white;
            border-radius: 10px;
            padding: 20px;
            box-shadow: 0 4px 6px rgba(0,0,0,0.1);
            width: 48%;
            transition: transform 0.3s ease;
        }
        .status-box:hover {
            transform: translateY(-5px);
        }
        table {
            width: 100%;
            border-collapse: separate;
            border-spacing: 0;
            margin-bottom: 20px;
            border-radius: 10px;
            overflow: hidden;
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
        .button-container {
            text-align: center;
            margin-top: 30px;
        }
        .button, input[type="submit"] {
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
            margin: 0 10px;
        }
        .button:hover, input[type="submit"]:hover {
            background-color: var(--secondary-color);
            transform: translateY(-2px);
            box-shadow: 0 4px 6px rgba(0,0,0,0.1);
        }
        input[type="text"], input[type="number"], select {
            width: 100%;
            padding: 12px;
            margin: 8px 0;
            display: inline-block;
            border: 1px solid var(--border-color);
            border-radius: 4px;
            box-sizing: border-box;
            transition: border-color 0.3s ease;
        }
        input[type="text"]:focus, input[type="number"]:focus, select:focus {
            border-color: var(--primary-color);
            outline: none;
        }
        .config-form {
            background-color: white;
            padding: 30px;
            border-radius: 10px;
            box-shadow: 0 4px 6px rgba(0,0,0,0.1);
        }
        .form-group {
            margin-bottom: 20px;
        }
        label {
            display: block;
            margin-bottom: 8px;
            font-weight: bold;
            color: var(--secondary-color);
        }
        input[type="checkbox"] {
            margin-right: 5px;
        }
        .auto-mode-container {
            background-color: white;
            border-radius: 10px;
            padding: 20px;
            box-shadow: 0 4px 6px rgba(0,0,0,0.1);
            margin-bottom: 30px;
        }
        .auto-mode-container h2 {
            margin-top: 0;
        }
        .auto-mode-container label {
            display: flex;
            align-items: center;
            font-weight: normal;
        }
        .auto-mode-container input[type="checkbox"] {
            margin-right: 10px;
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
        const STATUS_UPDATE_INTERVAL = 5000;

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
                })
                .catch(error => {
                    console.error("Error:", error);
                    document.getElementById("current-frequency").textContent = "Error updating";
                    document.getElementById("active-antenna").textContent = "Error updating";
                });
        }

        setInterval(updateStatus, STATUS_UPDATE_INTERVAL);
        updateStatus();
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

    ESP_LOGI(TAG, "Generating HTML for config: %d bands, %d antenna ports, tcp host:%s, tcp port:%d",
             config.num_bands, config.num_antenna_ports, config.tcp_host, config.tcp_port);
    ESP_LOGI(TAG, "Debug: num_bands = %d, num_antenna_ports = %d", config.num_bands, config.num_antenna_ports);

    ss << HTML_HEADER;
    ss << "<h2>Relay Configuration</h2>";
    ss << "<form id='configForm' class='config-form' onsubmit='submitConfig(event)'>";
    ss << "<div class='form-group'>";
    ss << "<label for='num_bands'>Number of bands:</label>";
    ss << "<input type='number' id='num_bands' name='num_bands' value='" << std::to_string(config.num_bands)
            << "' min='1' max='" << MAX_BANDS << "'>";
    ss << "</div>";
    ss << "<div class='form-group'>";
    ss << "<h3>Switch Configuration</h3>";
    ss << "<label for='num_antenna_ports'>Number of outputs:</label>";
    ss << "<input type='number' id='num_antenna_ports' name='num_antenna_ports' value='"
            << std::to_string(config.num_antenna_ports) << "' min='1' max='" << MAX_ANTENNA_PORTS << "'>";
    ss << "</div>";

    ss << "<div class='form-group'>";
    ss << "<h3>TCP Configuration</h3>";
    ss << "<label for='tcp_host'>TCP Host:</label>";
    ss << "<input type='text' id='tcp_host' name='tcp_host' value='" << config.tcp_host << "'>";
    ss << "</div>";
    ss << "<div class='form-group'>";
    ss << "<label for='tcp_port'>TCP Port:</label>";
    ss << "<input type='number' id='tcp_port' name='tcp_port' value='" << config.tcp_port << "' min='1' max='65535'>";
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
        ss << "<td><select name='band_" << i << "'>";

        for (const auto &[fst, snd]: band_info) {
            ss << "<option value='" << fst << "' "
                    << (config.bands[i].start_freq == snd.start_freq &&
                        config.bands[i].end_freq == snd.end_freq
                            ? "selected"
                            : "")
                    << ">" << snd.name << "</option>";
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
            uart_baud_rate: parseInt(formData.get('uart_baud_rate')),
            uart_parity: parseInt(formData.get('uart_parity')),
            uart_stop_bits: parseInt(formData.get('uart_stop_bits')),
            uart_flow_ctrl: parseInt(formData.get('uart_flow_ctrl')),
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
