import streamlit as st
import pandas as pd
import joblib
import altair as alt
from smart_energy_anomaly_prediction import fetch_last_10min_home_metrics, forecast_energy

# Streamlit page config
st.set_page_config(
    page_title="Smart Home Energy Analysis",
    layout="wide",
    initial_sidebar_state="expanded"
)

# Title
st.title("⚡ Smart Home Energy Analysis and Anomaly Detection")
st.markdown("This dashboard provides **real-time insights**, **energy forecasting**, and **anomaly detection** for your smart home.")

# Tabs
tabs = st.tabs(["📊 Dashboard", "🔮 Forecasting", "📎 Analysis"])

# ---------------------- 1. Dashboard Tab ----------------------
with tabs[0]:
    st.subheader("🔍 Live Anomaly Detection")

    appliance_options = {
        "Home (All Combined)": None,
        "Light 1 (Main)": "light1-main",
        "Light 2": "light2",
        "Socket": "socket1"
    }

    selected_appliance = st.selectbox("Select Appliance to Monitor for Anomalies:", list(appliance_options.keys()))

    if st.button("Show Anomalies"):
        with st.spinner("🔍 Detecting anomalies..."):
            df = fetch_last_10min_home_metrics(appliance=appliance_options[selected_appliance])

            if df.empty:
                st.warning("⚠️ No recent data found.")
            else:
                model = joblib.load("anomaliemodel.pkl")

                if 'power_error' not in df.columns:
                    df['expected_power'] = df['voltage'] * df['current'] * df['powerFactor']
                    df['power_error'] = abs(df['expected_power'] - df['power'])

                required_features = ["voltage", "current", "power", "powerFactor", "power_error"]
                if not all(f in df.columns for f in required_features):
                    st.error("❌ Required features missing from data.")
                else:
                    X = df[required_features]
                    df["anomaly"] = model.predict(X)
                    df["score"] = model.decision_function(X)

                    st.success("✅ Anomaly detection complete!")
                    st.markdown("### 📋 Detected Data")
                    st.dataframe(df[["timestamp", "current", "voltage", "power", "powerFactor", "power_error", "score", "anomaly"]])

                    anomalies = df[df['anomaly'] == -1]

                    if not anomalies.empty:
                        st.error("🚨 Anomalies Detected in the Last 10 Minutes")

                        st.markdown("""
                        ### 📌 See Anomalies!
                        - These data points significantly deviate from the usual behavior.
                        - 🔍 **Possible reasons include**:
                            - Sudden voltage fluctuations (voltage < 180V or > 250V)
                            - Unexpected current spikes
                            - Inefficient power usage (power factor < 0.6)
                            - Mismatch in actual vs expected power usage
                        """)

                        st.dataframe(anomalies[["timestamp", "voltage", "current", "power", "powerFactor", "power_error"]])

                        # Colored bar chart with Altair
                        st.markdown("### 📊 Anomaly Severity Chart")
                        chart_data = df[["timestamp", "score", "anomaly"]].copy()
                        chart_data["Status"] = chart_data["anomaly"].apply(lambda x: "Anomaly" if x == -1 else "Normal")

                        chart = alt.Chart(chart_data).mark_bar().encode(
                            x=alt.X("timestamp:T", title="Timestamp"),
                            y=alt.Y("score:Q", title="Anomaly Score"),
                            color=alt.Color("Status:N", scale=alt.Scale(domain=["Anomaly", "Normal"], range=["red", "green"])),
                            tooltip=["timestamp", "score", "Status"]
                        ).properties(
                            width="container",
                            height=300
                        )

                        st.altair_chart(chart, use_container_width=True)

                    else:
                        st.success("✅ No anomalies detected in the last 10 minutes.")

# ---------------------- 2. Forecasting Tab ----------------------
with tabs[1]:
    st.subheader("📈 7-Day Energy Forecast")

    if st.button("Show Forecast"):
        with st.spinner("📊 Generating forecast..."):
            try:
                model = joblib.load("energymodel.pkl")
                from prophet import Prophet

                future = model.make_future_dataframe(periods=7)
                forecast = model.predict(future)
                forecast = forecast[['ds', 'yhat', 'yhat_lower', 'yhat_upper']].tail(7)
                forecast.columns = ['Date', 'Predicted Energy (kWh)', 'Min Estimate', 'Max Estimate']
                forecast['Date'] = forecast['Date'].dt.strftime('%Y-%m-%d')
                forecast[['Predicted Energy (kWh)', 'Min Estimate', 'Max Estimate']] = forecast[
                    ['Predicted Energy (kWh)', 'Min Estimate', 'Max Estimate']].round(3)

                st.success("📈 Forecast generated successfully!")
                st.dataframe(forecast)
                st.line_chart(forecast.set_index('Date')[['Predicted Energy (kWh)']])
            except FileNotFoundError:
                st.error("❌ Energy model not found. Please train it first.")

# ---------------------- 3. Analysis Tab ----------------------
with tabs[2]:
    st.subheader("📎 Historical Anomaly Analysis (Last 10 Minutes)")

    if "df" in locals() and not df.empty:
        df["Status"] = df["anomaly"].apply(lambda x: "Anomaly" if x == -1 else "Normal")

        # 🔍 Anomaly Score Chart
        st.markdown("### 🔍 Anomaly Score Over Time")
        score_chart = alt.Chart(df).mark_line(point=alt.OverlayMarkDef(filled=True, size=80)).encode(
            x=alt.X("timestamp:T", title="Timestamp"),
            y=alt.Y("score:Q", title="Anomaly Score"),
            color=alt.Color("Status:N", scale=alt.Scale(domain=["Normal", "Anomaly"], range=["green", "red"])),
            tooltip=["timestamp:T", "score:Q", "Status:N"]
        ).properties(
            width="container",
            height=300,
            title="Anomaly Score vs Time"
        )

        st.altair_chart(score_chart, use_container_width=True)

        # ⚡ Line Chart for Power, Voltage, Current
        st.markdown("### ⚡ Voltage, Current, and Power Trends")
        metric_chart = alt.Chart(df).transform_fold(
            ["voltage", "current", "power"],
            as_=["Metric", "Value"]
        ).mark_line(point=True).encode(
            x=alt.X("timestamp:T", title="Timestamp"),
            y=alt.Y("Value:Q", title="Measured Value"),
            color=alt.Color("Metric:N", scale=alt.Scale(scheme='category10')),
            tooltip=["timestamp:T", "Metric:N", "Value:Q"]
        ).properties(
            width="container",
            height=300,
            title="Voltage, Current, and Power Over Time"
        )

        st.altair_chart(metric_chart, use_container_width=True)

    else:
        st.warning("⚠️ No data available. Please run anomaly detection in the Dashboard tab first.")