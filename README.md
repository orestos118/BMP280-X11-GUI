# BMP280-X11-GUI
A lightweight, X11-based graphical user interface for monitoring temperature and pressure data from a BMP280 sensor connected via a serial port. This project was developed with assistance from Grok 3, an AI platform by xAI, to provide real-time visualization and data logging capabilities.

Features

    Real-time plotting of temperature and pressure data.
    Interactive zooming, panning, and theme switching.
    Automatic serial port detection and reconnection.
    Data smoothing for cleaner graph visualization.
    CSV data logging with configurable intervals.
    Customizable via a configuration file (bmp280.ini).

License

This project is licensed under the GNU General Public License v3.0 (GPLv3). See the  file for details. You are free to use, modify, and distribute this software, provided that any derivative works are also licensed under GPLv3.

Requirements

    Operating System: Linux (or any system with X11 support).
    Dependencies:
        X11 development libraries (libx11-dev on Debian/Ubuntu).
        C++17-compatible compiler (e.g., g++).
    Hardware: A BMP280 sensor connected via a serial interface (e.g., /dev/ttyACM0 or /dev/ttyUSB0).

Installation

    Install Dependencies (on Debian/Ubuntu):
    bash

sudo apt update
sudo apt install libx11-dev g++
Clone the Repository:
bash
git clone https://github.com/yourusername/BMP280-X11-GUI.git
cd BMP280-X11-GUI
Compile the Code:
bash

    g++ -o bmp280_x11_gui5 bmp280_x11_gui5.cpp -lX11 -std=c++17

Usage

    Run the Program:
    bash

./bmp280_x11_gui5 [filename] [baud_rate] [delimiter]

    filename: Optional CSV output file (default: data_YYYYMMDD_HHMMSS.csv).
    baud_rate: Optional serial baud rate (default: 9600; supports 9600 or 115200).
    delimiter: Optional CSV delimiter (default: ,). Example:

bash

    ./bmp280_x11_gui5 sensor_data.csv 115200 ;
    Keyboard Shortcuts:
        q: Quit the application.
        s: Save data to a file (prompts for filename).
        p: Pause/resume data collection.
        c: Clear error messages.
        b: Change baud rate (prompts for input).
        +/-: Zoom in/out horizontally.
        Up/Down: Zoom in/out vertically.
        Left/Right: Scroll the graph.
        t: Toggle between White, Dark, and High-Contrast themes.
        h: Show/hide help menu.
    Mouse Controls:
        Left-click on graph: Zoom in.
        Right-click on graph: Zoom out.
        Middle-click and drag: Pan the graph.

Configuration

Edit bmp280.ini to customize settings (created automatically if not present):

    baud_rate: Serial baud rate (e.g., 9600 or 115200).
    save_interval: Data save interval in seconds (default: 30).
    temp_min/temp_max: Temperature range (default: -40 to 85).
    press_min/press_max: Pressure range (default: 300 to 1100).
    csv_delimiter: CSV delimiter (default: ,).
    menu_bg_color/help_bg_color: UI colors in hex (e.g., #808080).
    graph_color_*: Graph colors (e.g., blue, red).

Output

    Data is logged to logs/[filename] in CSV format: temperature,pressure,timestamp.
    Errors are logged to logs/errors.log.

Acknowledgments

    Developed with assistance from Grok 3, an AI platform by xAI, designed to accelerate human scientific discovery.
    Built for enthusiasts of sensor monitoring and X11 programming.

Contributing

Contributions are welcome! Feel free to fork this repository, submit pull requests, or open issues for bugs and feature requests. Please ensure any contributions adhere to the GPLv3 license.
