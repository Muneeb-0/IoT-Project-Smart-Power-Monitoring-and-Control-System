import pandas as pd
import joblib
from prophet import Prophet
from sklearn.neighbors import LocalOutlierFactor
from influxdb_client import InfluxDBClient
from datetime import datetime

# ------------------ InfluxDB Configuration ------------------
INFLUXDB_URL = "http://192.168.41.78:8086"
INFLUXDB_TOKEN = "yRTaoZbfgAxja1QghqdggMUqTZgjaCTSakNNZZtnzrmQp9SQobLH9MDhk7j5FYxFBc3TigbmO8M30QdHo52Q5g=="
INFLUXDB_ORG = "iotlab"
INFLUXDB_BUCKET = "smart_power_analysis"

# ------------------ 1. Fetch Daily Energy Summary ------------------
def fetch_daily_energy():
    client = InfluxDBClient(url=INFLUXDB_URL, token=INFLUXDB_TOKEN, org=INFLUXDB_ORG)
    query_api = client.query_api()
    query = f'''
    from(bucket: "{INFLUXDB_BUCKET}")
      |> range(start: -30d)
      |> filter(fn: (r) => r._measurement == "home_metrics" and r._field == "energy")
      |> aggregateWindow(every: 1d, fn: last, createEmpty: false)
      |> pivot(rowKey:["_time"], columnKey: ["_field"], valueColumn: "_value")
      |> keep(columns: ["_time", "energy"])
    '''
    df = query_api.query_data_frame(query)
    if not df.empty:
        df.rename(columns={"_time": "timestamp"}, inplace=True)
        df['timestamp'] = pd.to_datetime(df['timestamp']).dt.tz_localize(None)
        df['daily_energy'] = df['energy'].diff()
        df = df.dropna()
    return df[['timestamp', 'daily_energy']]

# ------------------ 2. Train & Save Prophet Model ------------------
def train_energy_model(df):
    df_prophet = df.rename(columns={"timestamp": "ds", "daily_energy": "y"})
    model = Prophet()
    model.fit(df_prophet)
    joblib.dump(model, "energymodel.pkl")
    print("✅ Energy forecast model saved as energymodel.pkl")

# ------------------ 3. Forecast Using Saved Model ------------------
def forecast_energy():
    model = joblib.load("energymodel.pkl")
    future = model.make_future_dataframe(periods=7)
    forecast = model.predict(future)
    forecast = forecast[['ds', 'yhat', 'yhat_lower', 'yhat_upper']].tail(7)
    forecast.columns = ['Date', 'Predicted Energy (kWh)', 'Min Estimate', 'Max Estimate']
    forecast['Date'] = forecast['Date'].dt.strftime('%Y-%m-%d')
    forecast[['Predicted Energy (kWh)', 'Min Estimate', 'Max Estimate']] = forecast[
        ['Predicted Energy (kWh)', 'Min Estimate', 'Max Estimate']].round(3)
    print("\n📊 7-Day Forecast of Home Energy Usage:")
    print(forecast.to_string(index=False))

# ------------------ 4. Fetch Training Data for Anomaly Detection ------------------
def fetch_training_anomaly_data():
    client = InfluxDBClient(url=INFLUXDB_URL, token=INFLUXDB_TOKEN, org=INFLUXDB_ORG)
    query_api = client.query_api()
    query = f'''
    from(bucket: "{INFLUXDB_BUCKET}")
      |> range(start: -7d)
      |> filter(fn: (r) => r._measurement == "home_metrics")
      |> filter(fn: (r) => r._field == "current" or r._field == "power" or r._field == "voltage" or r._field == "powerFactor")
      |> aggregateWindow(every: 5m, fn: mean, createEmpty: false)
      |> pivot(rowKey:["_time"], columnKey: ["_field"], valueColumn: "_value")
      |> keep(columns: ["_time", "current", "power", "voltage", "powerFactor"])
    '''
    df = query_api.query_data_frame(query)
    if not df.empty:
        df.rename(columns={"_time": "timestamp"}, inplace=True)
        df['timestamp'] = pd.to_datetime(df['timestamp'])
        df['expected_power'] = df['voltage'] * df['current'] * df['powerFactor']
        df['power_error'] = abs(df['expected_power'] - df['power'])
    return df

# ------------------ 5. Train & Save Anomaly Detection Model ------------------
def train_anomaly_model(df):
    X = df[['voltage', 'current', 'power', 'powerFactor', 'power_error']]
    model = LocalOutlierFactor(n_neighbors=7, contamination=0.05, novelty=True)
    model.fit(X)
    joblib.dump(model, "anomaliemodel.pkl")
    print("✅ Anomaly detection model saved as anomaliemodel.pkl")

def fetch_last_10min_home_metrics(appliance=None):
    from influxdb_client import InfluxDBClient
    import pandas as pd

    client = InfluxDBClient(url=INFLUXDB_URL, token=INFLUXDB_TOKEN, org=INFLUXDB_ORG)
    query_api = client.query_api()

    if appliance:
        # Fetch current + energy from appliance_metrics
        appliance_query = f'''
        from(bucket: "{INFLUXDB_BUCKET}")
          |> range(start: -10m)
          |> filter(fn: (r) => r._measurement == "appliance_metrics")
          |> filter(fn: (r) => r["appliance"] == "{appliance}")
          |> filter(fn: (r) => r._field == "current" or r._field == "energy")
          |> aggregateWindow(every: 1m, fn: mean, createEmpty: false)
          |> pivot(rowKey:["_time"], columnKey: ["_field"], valueColumn: "_value")
          |> keep(columns: ["_time", "current", "energy"])
        '''
        df_appliance = query_api.query_data_frame(appliance_query)
        if df_appliance.empty:
            return pd.DataFrame()
        df_appliance.rename(columns={"_time": "timestamp"}, inplace=True)
        df_appliance['timestamp'] = pd.to_datetime(df_appliance['timestamp'])

        # Fetch latest voltage, power, power factor from home_metrics
        home_query = f'''
        from(bucket: "{INFLUXDB_BUCKET}")
          |> range(start: -10m)
          |> filter(fn: (r) => r._measurement == "home_metrics")
          |> filter(fn: (r) => r._field == "voltage" or r._field == "power" or r._field == "powerFactor")
          |> aggregateWindow(every: 1m, fn: mean, createEmpty: false)
          |> pivot(rowKey:["_time"], columnKey: ["_field"], valueColumn: "_value")
          |> keep(columns: ["_time", "voltage", "power", "powerFactor"])
        '''
        df_home = query_api.query_data_frame(home_query)
        if df_home.empty:
            return pd.DataFrame()
        df_home.rename(columns={"_time": "timestamp"}, inplace=True)
        df_home['timestamp'] = pd.to_datetime(df_home['timestamp'])

        # Take latest home metrics and add them to all appliance rows
        latest_home = df_home.sort_values("timestamp").iloc[-1][["voltage", "power", "powerFactor"]]
        for col in ['voltage', 'power', 'powerFactor']:
            df_appliance[col] = latest_home[col]

        # Compute expected power and power error
        df_appliance['expected_power'] = df_appliance['voltage'] * df_appliance['current'] * df_appliance['powerFactor']
        df_appliance['power_error'] = abs(df_appliance['expected_power'] - df_appliance['power'])

        return df_appliance

    else:
        # Home-level metrics (use actual power readings)
        home_query = f'''
        from(bucket: "{INFLUXDB_BUCKET}")
          |> range(start: -10m)
          |> filter(fn: (r) => r._measurement == "home_metrics")
          |> filter(fn: (r) => r._field == "current" or r._field == "power" or r._field == "voltage" or r._field == "powerFactor")
          |> aggregateWindow(every: 1m, fn: mean, createEmpty: false)
          |> pivot(rowKey:["_time"], columnKey: ["_field"], valueColumn: "_value")
          |> keep(columns: ["_time", "current", "power", "voltage", "powerFactor"])
        '''
        df = query_api.query_data_frame(home_query)
        if df.empty:
            return pd.DataFrame()
        df.rename(columns={"_time": "timestamp"}, inplace=True)
        df['timestamp'] = pd.to_datetime(df['timestamp'])

        # Compute expected power and error
        df['expected_power'] = df['voltage'] * df['current'] * df['powerFactor']
        df['power_error'] = abs(df['expected_power'] - df['power'])

        return df

# ------------------ 7. Predict Anomalies Using Saved Model ------------------
def detect_recent_anomalies():
    df = fetch_last_10min_home_metrics()
    if df.empty:
        print("⚠️ No recent data found.")
        return
    model = joblib.load("anomaliemodel.pkl")
    X = df[['voltage', 'current', 'power', 'powerFactor', 'power_error']]
    df['anomaly'] = model.predict(X)
    df['score'] = model.decision_function(X)
    print(df[['timestamp', 'current', 'voltage', 'power', 'powerFactor', 'power_error', 'score', 'anomaly']])
    anomalies = df[df['anomaly'] == -1]
    if not anomalies.empty:
        print("\n🚨 Recent Anomalies (last 10 minutes):")
        print(anomalies[['timestamp', 'voltage', 'current', 'power', 'powerFactor', 'power_error']])
    else:
        print("\n✅ No anomalies detected in the last 10 minutes.")

# ------------------ Main ------------------
if __name__ == "__main__":
    print("\n========================= ⚡ HOME ENERGY FORECAST =========================")
    df_energy = fetch_daily_energy()
    if not df_energy.empty:
        train_energy_model(df_energy)
        forecast_energy()
    else:
        print("⚠️ No daily energy data available.")

    print("\n========================= 🔍 ANOMALY DETECTION =========================")
    df_anomaly = fetch_training_anomaly_data()
    if not df_anomaly.empty:
        train_anomaly_model(df_anomaly)
        detect_recent_anomalies()
    else:
        print("⚠️ No training data for anomaly model.")
