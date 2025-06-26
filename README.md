# ⚡ Smart Energy Analysis & Control IoT Project

Welcome to the **Smart Energy Analysis & Control IoT Project**!  
This system provides **real-time energy monitoring**, **anomaly detection**, and **AI-powered forecasting** for smart homes. It combines robust **IoT hardware** with advanced **machine learning** to help you optimize energy usage, detect faults, and control appliances efficiently.

---

## 🚀 Features

- **Live Energy Monitoring**: Track voltage, current, power, and energy consumption for your entire home and individual appliances.
- **Anomaly Detection**: Instantly spot abnormal energy patterns using machine learning.
- **7-Day Forecasting**: Predict future energy usage with AI (Prophet model).
- **Historical Analysis**: Visualize trends and past anomalies.
- **Remote Appliance Control**: Manage relays for lights and sockets.
- **Beautiful Dashboard**: Interactive Streamlit web app for all analytics.
- **Cloud Data Storage**: All metrics are stored in **InfluxDB** for reliability and scalability.

---

## 🏗️ System Architecture

![Editor _ Mermaid Chart-2025-06-26-171329](https://github.com/user-attachments/assets/ab505b5a-9a7d-4a65-b621-e547c88f7281)


---

## 🛠️ Installation & Setup

### 1. Hardware

- **Microcontroller**: ESP32
- **Sensors**: PZEM-004T (voltage, current, power, energy), ACS712 (current)
- **Display**: OLED (SSD1306)
- **Relays**: For appliance control
- **Connectivity**: WiFi

### 2. Firmware

- Flash the `Smart_energry_monitering_IoT.ino` sketch to your **ESP32**.
- Configure your **WiFi credentials** and **InfluxDB endpoint** in the code.

### 3. Backend & Dashboard

#### Prerequisites

- Python **3.8+**
- InfluxDB instance (local or cloud)

#### Install Dependencies

pip install -r requirements.txt

## 🤖 Machine Learning Integration

### Anomaly Detection
Uses a **Local Outlier Factor (LOF)** model trained on recent home metrics.

### Forecasting
Uses **Facebook Prophet** for 7-day energy predictions.

---

## 🧠 Model Files

- `anomaliemodel.pkl`
- `energymodel.pkl`

---

## 🗂️ File Structure

```text
├── firmware/
│   └── Smart_energry_monitering_IoT.ino
├── dashboard/
│   ├── dashboard.py
│   ├── anomaliemodel.pkl
│   └── energymodel.pkl
├── requirements.txt
├── README.md
├── Presentation - Smart Energy Monitoring System.pdf
└── IoT_Fundamentals_Project_Documentation_1368.pdf

---

## 🧰 Hardware Requirements

- ESP32 Dev Board  
- PZEM-004T v3.0  
- ACS712 Current Sensors  
- SSD1306 OLED Display  
- 3x Relays  
- WiFi Network  
- InfluxDB Server  

---

## 🤝 Contributing

Pull requests are welcome!  
For major changes, please open an issue first to discuss what you'd like to change.

---

## 📄 License

This project is intended for educational and research purposes.

---

## 📬 Contact

**Project Lead**: [Muneeb Ur Rehman or urrehnammuneeb66@gmail.com]  
**Documentation**: See `IoT_Fundamentals_Project_Documentation_1368.pdf`  
**Presentation**: See `Presentation - Smart Energy Monitoring System.pdf`
