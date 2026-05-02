#include <Arduino.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <sys/time.h>

String alarm1, alarm2;
Preferences prefs;
DNSServer dnsServer;
WebServer server(80);

constexpr char INDEX_HTML[] PROGMEM = R"=====(
<!DOCTYPE html>
<html>
<head>
    <meta charset="utf-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>PillBox Setup</title>
    <style>
        body { font-family: -apple-system, sans-serif; background: #f4f4f9; display: flex; flex-direction: column; align-items: center; padding: 20px; }
        .card { background: white; padding: 20px; border-radius: 15px; box-shadow: 0 4px 6px rgba(0,0,0,0.1); width: 100%; max-width: 350px; }
        h1 { color: #333; font-size: 20px; text-align: center; }
        .input-group { margin-bottom: 15px; }
        label { display: block; margin-bottom: 5px; color: #666; font-size: 14px; }
        input[type="time"] { width: 100%; padding: 10px; border: 1px solid #ddd; border-radius: 8px; font-size: 16px; box-sizing: border-box; }
        button { width: 100%; background: #4CAF50; color: white; border: none; padding: 12px; border-radius: 8px; font-size: 16px; cursor: pointer; transition: 0.3s; }
        button:active { background: #45a049; transform: scale(0.98); }
        #status { margin-top: 15px; font-size: 12px; text-align: center; color: #888; }
    </style>
</head>
<body>
    <div class="card">
        <h1>Настройка Таблетницы 💊</h1>

        <div class="input-group">
            <label>Утренний прием:</label>
            <input type="time" id="time1" value="08:00">
        </div>

        <div class="input-group">
            <label>Вечерний прием:</label>
            <input type="time" id="time2" value="20:00">
        </div>

        <button onclick="saveSettings()">Сохранить настройки</button>
        <div id="status"></div>
    </div>

    <script>
        function saveSettings() {
            const t1 = document.getElementById('time1').value;
            const t2 = document.getElementById('time2').value;

            // Получаем текущее время телефона в Unix формате (секунды)
            const nowSeconds = Math.floor(Date.now() / 1000);

            // Формируем URL с параметрами
            const url = `/save?t1=${t1}&t2=${t2}&now=${nowSeconds}`;

            document.getElementById('status').innerText = "Сохранение...";

            fetch(url).then(response => {
                if (response.ok) {
                    document.getElementById('status').innerText = "Настройки сохранены! Время синхронизировано.";
                    document.getElementById('status').style.color = "green";
                }
            });
        }
    </script>
</body>
</html>
)=====";

void handleSave()
{
	if (server.hasArg("t1") && server.hasArg("t2") && server.hasArg("now"))
	{
		const String t1 = server.arg("t1");
		const String t2 = server.arg("t2");
		const long phoneTime = server.arg("now").toInt();

		// 1. Синхронизируем внутреннее время ESP32
		const timeval epoch = {phoneTime, 0};
		settimeofday(&epoch, nullptr);

		// 2. Выводим в лог для проверки
		Serial.println("--- Новые настройки ---");
		Serial.println("Утро: " + t1);
		Serial.println("Вечер: " + t2);

		// Проверим, что время установилось
		time_t now;
		tm timeinfo{};
		time(&now);
		localtime_r(&now, &timeinfo);
		Serial.printf("Текущее время ESP32: %02d:%02d:%02d\n", timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);

		server.send(200, "text/plain", "OK");
	} else { server.send(400, "text/plain", "Bad Request"); }
}

void setup()
{
	Serial.begin(115200);

	setenv("TZ", "UTC-6", 1);
	tzset();

	WiFiClass::mode(WIFI_AP);
	WiFi.softAP("PillBox_Setup");

	dnsServer.start(53, "*", WiFi.softAPIP());

	server.on("/", []() { server.send(200, "text/html", INDEX_HTML); });

	// Регистрируем обработчик для сохранения
	server.on("/save", handleSave);

	server.onNotFound([]() {
		server.sendHeader("Location", "/", true);
		server.send(302, "text/plain", "");
	});

	server.begin();
	Serial.println("Server started!");
}

void loop()
{
	dnsServer.processNextRequest();
	server.handleClient();
}
