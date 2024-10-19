#include "html_content.h"
#include <sstream>
#include <esp_log.h>

const char* TAG = "HTML";
const char* HTML_HEADER = R"(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Antenna Switch Controller</title>
    <style>
        body {
            font-family: Arial, sans-serif;
            line-height: 1.6;
            color: #333;
            max-width: 800px;
            margin: 0 auto;
            padding: 20px;
            background-color: #f4f4f4;
        }
        h1, h2 {
            color: #2c3e50;
        }
        .status-container {
            display: flex;
            justify-content: space-between;
            margin-bottom: 20px;
        }
        .status-box {
            background-color: white;
            border-radius: 5px;
            padding: 15px;
            box-shadow: 0 2px 5px rgba(0,0,0,0.1);
            width: 48%;
        }
        table {
            width: 100%;
            border-collapse: collapse;
            margin-bottom: 20px;
        }
        th, td {
            padding: 12px;
            border-bottom: 1px solid #ddd;
            text-align: left;
        }
        th {
            font-weight: bold;
            color: #2c3e50;
            background-color: #ecf0f1;
        }
        .button-container {
            text-align: center;
            margin-top: 20px;
        }
        .button, input[type="submit"] {
            display: inline-block;
            background-color: #3498db;
            color: white;
            padding: 10px 20px;
            border-radius: 5px;
            text-decoration: none;
            transition: background-color 0.3s;
            border: none;
            cursor: pointer;
            font-size: 16px;
        }
        .button:hover, input[type="submit"]:hover {
            background-color: #2980b9;
        }
        input[type="text"], input[type="number"], select {
            width: 100%;
            padding: 8px;
            margin: 5px 0;
            display: inline-block;
            border: 1px solid #ccc;
            border-radius: 4px;
            box-sizing: border-box;
        }
        .config-form {
            background-color: white;
            padding: 20px;
            border-radius: 5px;
            box-shadow: 0 2px 5px rgba(0,0,0,0.1);
        }
        .form-group {
            margin-bottom: 15px;
        }
        label {
            display: block;
            margin-bottom: 5px;
            font-weight: bold;
        }
    </style>
</head>
<body>
)";

const char* HTML_FOOTER = R"(
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
    {"2m", {"2m", 144000000, 148000000}},
    {"70cm", {"70cm", 420000000, 450000000}}
};

std::string generate_root_html(const antenna_switch_config_t& config, const char* ip_addr) {
    std::stringstream ss;
    ss << HTML_HEADER;
    ss << R"(
    <h1>Antenna Switch Controller</h1>
    <div class="status-container">
        <div class="status-box">
            <h2>Current Status</h2>
            <table>
                <tr><th>Current Frequency</th><td id="current-frequency">Updating...</td></tr>
                <tr><th>Active Antenna Port</th><td id="active-antenna">Updating...</td></tr>
            </table>
        </div>
        <div class="status-box">
            <h2>Network Information</h2>
            <table>
                <tr><th>IP Address</th><td>)" << ip_addr << R"(</td></tr>
            </table>
        </div>
    </div>
    <div class="status-box" style="width: 100%; margin-top: 20px;">
        <h2>Auto Mode</h2>
        <form action='/toggle-auto-mode' method='post'>
            <label>
                <input type='checkbox' name='auto_mode' )" << (config.auto_mode ? "checked" : "") << R"( onchange='this.form.submit()'>
                Enable Automatic band selection 
            </label>
        </form>
    </div>
    <div class="button-container">
        <a href='/config' class="button">Edit Configuration</a>
    </div>
    <script>
        function updateStatus() {
            fetch('/status')
                .then(response => response.json())
                .then(data => {
                    document.getElementById('current-frequency').textContent = data.frequency + ' Hz';
                    document.getElementById('active-antenna').textContent = data.antenna;
                })
                .catch(error => console.error('Error:', error));
        }
        setInterval(updateStatus, 5000);
        updateStatus();
    </script>
    )";
    ss << HTML_FOOTER;
    return ss.str();
}

std::string generate_config_html(const antenna_switch_config_t& config) {
    std::stringstream ss;
    
    // Check for potential errors before starting to generate HTML
    if (config.num_bands <= 0 || config.num_bands > MAX_BANDS) {
        ESP_LOGE(TAG, "Invalid number of bands: %d (should be between 1 and %d)", 
                 config.num_bands, MAX_BANDS);
        return "";  // Return empty string to indicate error
    }
    if (config.num_antenna_ports <= 0 || config.num_antenna_ports > MAX_ANTENNA_PORTS) {
        ESP_LOGE(TAG, "Invalid number of antenna ports: %d (should be between 1 and %d)", 
                 config.num_antenna_ports, MAX_ANTENNA_PORTS);
        return "";  // Return empty string to indicate error
    }

    ESP_LOGI(TAG, "Generating HTML for config with %d bands and %d antenna ports", 
             config.num_bands, config.num_antenna_ports);

    ss << HTML_HEADER;
    ss << R"(
    <h2>Antenna Switch Configuration</h2>
    <form action='/config' method='post' class="config-form">
        <div class="form-group">
            <label for="num_bands">Number of bands:</label>
            <input type='number' id="num_bands" name='num_bands' value=')" << config.num_bands << R"(' min='1' max=')" << MAX_BANDS << R"('>
        </div>
        <div class="form-group">
            <label for="num_antenna_ports">Number of antenna ports:</label>
            <input type='number' id="num_antenna_ports" name='num_antenna_ports' value=')" << config.num_antenna_ports << R"(' min='1' max=')" << MAX_ANTENNA_PORTS << R"('>
        </div>
        <table>
            <thead>
                <tr>
                    <th>Band</th>
                    <th>Start Freq</th>
                    <th>End Freq</th>
                    <th>Antenna Ports</th>
                </tr>
            </thead>
            <tbody>
    )";

    for (int i = 0; i < config.num_bands; i++) {
        ss << "<tr>"
              "<td><select name='band_" << i << "'>";
        
        for (const auto& band : band_info) {
            ss << "<option value='" << band.first << "' " << 
                  (config.bands[i].start_freq == band.second.start_freq && config.bands[i].end_freq == band.second.end_freq ? "selected" : "") << 
                  ">" << band.second.name << "</option>";
        }
        
        ss << "</select></td>"
              "<td>" << config.bands[i].start_freq << "</td>"
              "<td>" << config.bands[i].end_freq << "</td>"
              "<td>";
        
        // Generate checkboxes for each antenna port
        for (int j = 0; j < config.num_antenna_ports; j++) {
            ss << "<label><input type='checkbox' name='antenna_" << i << "_" << j << "' " 
               << (config.bands[i].antenna_ports[j] ? "checked" : "") << "> Port " << (j + 1) << "</label> ";
        }
        
        ss << "</td></tr>";
    }

    ss << R"(
            </tbody>
        </table>
        <div class="button-container">
            <input type='submit' value='Update Configuration'>
        </div>
    </form>
    <div class="button-container">
        <a href='/' class="button">Back to Home</a>
        <form action='/reset-config' method='post' style="display: inline;">
            <input type='submit' value='Reset Configuration' class="button" style="background-color: #e74c3c;" onclick="return confirm('Are you sure you want to reset the configuration?');">
        </form>
    </div>
    )";
    ss << HTML_FOOTER;
    return ss.str();
}
