import asyncio
import json
from winsdk.windows.devices.geolocation import Geolocator
import paho.mqtt.client as mqtt

# ===== THINGSBOARD =====
TB_HOST = "demo.thingsboard.io"  # hoặc "your.thingsboard.cloud"
ACCESS_TOKEN = "ju7vl5WQZY29H61RRgnM"

client = mqtt.Client()
client.username_pw_set(ACCESS_TOKEN)
client.connect(TB_HOST, 1883, 60)
client.loop_start()  # Quan trọng: giữ kết nối sống

async def get_location():
    locator = Geolocator()
    while True:
        try:
            pos = await locator.get_geoposition_async()
            lat = pos.coordinate.point.position.latitude
            lon = pos.coordinate.point.position.longitude
            payload = {"latitude": lat, "longitude": lon}
            client.publish("v1/devices/me/telemetry", json.dumps(payload))
            print(f"Đã gửi: Lat={lat}, Lon={lon}")
        except Exception as e:
            print("Lỗi lấy vị trí hoặc gửi MQTT:", e)
        await asyncio.sleep(5)

try:
    asyncio.run(get_location())
except KeyboardInterrupt:
    client.loop_stop()
    client.disconnect()
    print("Đã dừng.")