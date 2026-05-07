#include <Arduino.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <sys/time.h>

constexpr int LED_PIN = 2;
constexpr int LID_SENSOR_PIN = 4;

String alarm1, alarm2;
String lastTriggeredMinute = "";
bool isAlerting = false;

Preferences prefs;
DNSServer dnsServer;
WebServer server(80);
unsigned long lastCheckTime = 0;
unsigned long lastBlinkTime = 0;
bool ledState = false;

constexpr char INDEX_HTML[] PROGMEM = R"=====(
<!DOCTYPE html>
<html>
<head>
	<meta charset="utf-8">
	<meta name="viewport" content="width=device-width, initial-scale=1.0">
	<title>PillBox</title>
	<style>
		body {
			font-family:sans-serif;
			background:#f4f4f9;
			display:flex;
			flex-direction:column;
			align-items:center;
			padding:20px;
		}
		.card {
			background:white;
			padding:20px;
			border-radius:15px;
			box-shadow:0 4px 6px rgba(0,0,0,0.1);
			width:100%;
			max-width:300px;
		}
		input {
			width:100%;
			padding:10px;
			margin:10px 0;
			border:1px solid #ddd;
			border-radius:8px;
			box-sizing:border-box;
		}
		button {
			width:100%;
			background:#4CAF50;
			color:white;border:none;
			padding:12px;
			border-radius:8px;
			width:100%;
		}
	</style>
</head>
<body>
	<div class="card">
		<h2>Настройка 💊</h2>
		<label>Утро:</label>
		<input type="time" id="t1">
		<br><label>Вечер:</label>
		<input type="time" id="t2">
		<button onclick="save()">Сохранить</button>
		<p id="status"></p>
	</div>

	<script>
		function save() {
			const t1 = document.getElementById('t1').value;
			const t2 = document.getElementById('t2').value;
			const now = Math.floor(Date.now() / 1000);

			fetch(`/save?t1=${t1}&t2=${t2}&now=${now}`)
				.then(() => document.getElementById('status').innerText="Сохранено!" );
		}
	</script>
</body>
</html>
)=====";

void handleSave()
{
	for (int i = 0; i < server.args(); i++)
	{
		Serial.printf("Аргумент [%s]: %s\n", server.argName(i).c_str(), server.arg(i).c_str());
	}

	if (server.hasArg("t1") && server.hasArg("t2") && server.hasArg("now"))
	{
		alarm1 = server.arg("t1");
		alarm2 = server.arg("t2");
		const long phoneTime = server.arg("now").toInt();

		const timeval epoch = {phoneTime, 0};
		settimeofday(&epoch, nullptr);

		prefs.begin("pillbox", false);
		prefs.putString("t1", alarm1);
		prefs.putString("t2", alarm2);
		prefs.end();

		Serial.println("Результат: Успешно сохранено!");
		server.send(200, "text/plain", "OK");
	}
	else
	{
		Serial.println("Результат: Ошибка! Не все аргументы получены.");
		server.send(400, "text/plain", "Missing arguments");
	}
	Serial.flush();
}

void setup()
{
	Serial.begin(115200);
	setenv("TZ", "UTC-8", 1);
	tzset();

	pinMode(LED_PIN, OUTPUT);
	pinMode(LID_SENSOR_PIN, INPUT_PULLUP); // подтяжка

	prefs.begin("pillbox", true);
	alarm1 = prefs.getString("t1", "08:00");
	alarm2 = prefs.getString("t2", "20:00");
	prefs.end();

	WiFiClass::mode(WIFI_AP);
	WiFi.softAP("PillBox_Setup");
	dnsServer.start(53, "*", WiFi.softAPIP());
	server.on("/", [] {
		server.send(200, "text/html", INDEX_HTML);
	});
	server.on("/save", handleSave);
	server.onNotFound([] {
		server.sendHeader("Location", "/", true);
		server.send(302, "text/plain", "");
	});
	server.begin();
}

void loop()
{
	dnsServer.processNextRequest();
	server.handleClient();

	// 1. Проверка времени
	if (millis() - lastCheckTime > 1000)
	{
		lastCheckTime = millis();
		time_t now;
		tm ti{};
		time(&now);
		localtime_r(&now, &ti);

		if (ti.tm_year > 70)
		{
			char buf[6];
			sprintf(buf, "%02d:%02d", ti.tm_hour, ti.tm_min);
			const auto cur = String(buf);

			// Если настало время И мы еще не срабатывали в эту минуту
			if ((cur == alarm1 || cur == alarm2) && cur != lastTriggeredMinute)
			{
				isAlerting = true;
				lastTriggeredMinute = cur;
				Serial.println("Пора пить таблетки!");
			}
		}
	}

	// 2. Логика сброса
	if (digitalRead(LID_SENSOR_PIN) == HIGH)
	{
		if (isAlerting)
		{
			isAlerting = false;
			digitalWrite(LED_PIN, LOW);
			Serial.println("Крышка открыта, таблетки взяты.");
			delay(200);
		}
	}

	// 3. Мигание светодиодом
	if (isAlerting)
	{
		if (millis() - lastBlinkTime > 500)
		{
			lastBlinkTime = millis();
			ledState = !ledState;
			digitalWrite(LED_PIN, ledState);
		}
	}
	else
	{
		digitalWrite(LED_PIN, LOW);
	}
}
