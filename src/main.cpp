#include <Arduino.h>
#include <ArduinoJson.h>
#include <DNSServer.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <sys/time.h>

const String BOT_TOKEN = "7764936622:AAGf0k9Ol6ZpSg0V_mUU6eNRuWRDPd5QH94";
const String G_SCRIPT_URL =
		"https://script.google.com/macros/s/AKfycbxgWAfNDyaRgaCFu3RDdx3NPYxIx9goPPmhlKp-aSIVyBxRP-h2AVnNRICZCzFf3wQG/exec";

constexpr int LID_SENSOR_PIN = 4;
constexpr int BUZZER_PIN = 0;

String ssid, pass, chatId, alarm1, alarm2;
volatile bool isAlerting = false;
String lastTriggeredMinute = "";
unsigned long lastBotCheck = 0;
long lastUpdateId = 0;

Preferences prefs;
DNSServer dnsServer;
WebServer server(80);

[[noreturn]] void buzzerTask(void *pvParameters)
{
	for (;;)
	{
		if (isAlerting)
		{
			for (int i = 0; i < 30; i++)
			{
				if (!isAlerting) break;
				digitalWrite(BUZZER_PIN, HIGH);
				vTaskDelay(10 / portTICK_PERIOD_MS);
			}
			digitalWrite(BUZZER_PIN, LOW);

			for (int i = 0; i < 80; i++)
			{
				if (!isAlerting) break;
				vTaskDelay(10 / portTICK_PERIOD_MS);
			}
		}
		else
		{
			digitalWrite(BUZZER_PIN, LOW);
			vTaskDelay(50 / portTICK_PERIOD_MS);
		}
	}
}

String callTelegram(const String &method, const String &params)
{
	if (WiFiClass::status() != WL_CONNECTED) return "";
	WiFiClientSecure client;
	client.setInsecure();

	HTTPClient http;
	String url = G_SCRIPT_URL + "?token=" + BOT_TOKEN + "&method=" + method + "&" + params;
	url.replace("\n", "%0A");
	url.replace(" ", "%20");

	http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
	http.setTimeout(10000);
	String response = "";
	if (http.begin(client, url))
	{
		int httpCode = http.GET();
		if (httpCode == 200) response = http.getString();
		http.end();
	}
	return response;
}

void sendMsg(const String &text)
{
	callTelegram("sendMessage", "chat_id=" + chatId + "&text=" + text);
}

void checkBot()
{
	String json = callTelegram("getUpdates", "offset=" + String(lastUpdateId + 1));
	if (json == "" || json.length() < 10) return;
	DynamicJsonDocument doc(2048);
	if (deserializeJson(doc, json)) return;
	JsonArray results = doc["result"].as<JsonArray>();
	for (JsonObject result: results)
	{
		lastUpdateId = result["update_id"];
		String text = result["message"]["text"].as<String>();
		String fromId = result["message"]["chat"]["id"].as<String>();
		fromId.trim();
		chatId.trim();
		if (fromId == chatId)
		{
			if (text.startsWith("/t1 "))
			{
				alarm1 = text.substring(4, 9);
				sendMsg("✅ Утро: " + alarm1);
			}
			else if (text.startsWith("/t2 "))
			{
				alarm2 = text.substring(4, 9);
				sendMsg("✅ Вечер: " + alarm2);
			}
			else if (text == "/status")
			{
				time_t now = time(nullptr);
				const tm *ti = localtime(&now);
				char b[10];
				sprintf(b, "%02d:%02d:%02d", ti->tm_hour, ti->tm_min, ti->tm_sec);
				String lid = digitalRead(LID_SENSOR_PIN) == HIGH ? "ЗАКРЫТА" : "ОТКРЫТА";
				sendMsg(
					"📊 СТАТУС:\n🕒 Время: " + String(b) + "\n🌅 Утро: " + alarm1 + "\n🌌 Вечер: " + alarm2 +
					"\n📦 Крышка: " + lid
				);
			}
			prefs.begin("pillbox", false);
			prefs.putString("t1", alarm1);
			prefs.putString("t2", alarm2);
			prefs.end();
		}
	}
}

constexpr char INDEX_HTML[] PROGMEM = R"=====(
<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1.0">
<style>body{font-family:sans-serif;background:#f4f4f9;padding:20px;display:flex;flex-direction:column;align-items:center;}
.card{background:white;padding:25px;border-radius:15px;box-shadow:0 4px 10px rgba(0,0,0,0.1);width:100%;max-width:320px;}
input{width:100%;padding:12px;margin-top:10px;border:1px solid #ddd;border-radius:8px;box-sizing:border-box;font-size:16px;}
button{width:100%;background:#4CAF50;color:white;border:none;padding:15px;border-radius:8px;margin-top:20px;width:100%;}</style></head>
<body><div class="card"><h2>PillBox Setup 💊</h2>
<input id="s" placeholder="WiFi Name"><input type="password" id="p" placeholder="WiFi Password"><input id="c" placeholder="Chat ID">
<label>Утро:</label><input type="time" id="t1" value="08:00"><label>Вечер:</label><input type="time" id="t2" value="20:00">
<button onclick="save()">СОХРАНИТЬ</button></div>
<script>function save(){
const q=`s=${document.getElementById('s').value}&p=${document.getElementById('p').value}&c=${document.getElementById('c').value}&t1=${document.getElementById('t1').value}&t2=${document.getElementById('t2').value}`;
fetch('/save?'+q).then(()=>alert("Перезагрузка..."));
}</script></body></html>
)=====";

void handleSave()
{
	prefs.begin("pillbox", false);
	prefs.putString("s", server.arg("s"));
	prefs.putString("p", server.arg("p"));
	prefs.putString("c", server.arg("c"));
	prefs.putString("t1", server.arg("t1"));
	prefs.putString("t2", server.arg("t2"));
	prefs.end();
	server.send(200, "text/plain", "OK");
	delay(2000);
	ESP.restart();
}

void setup()
{
	Serial.begin(115200);
	setenv("TZ", "UTC-8", 1);
	tzset();
	pinMode(BUZZER_PIN, OUTPUT);
	digitalWrite(BUZZER_PIN, LOW);
	pinMode(LID_SENSOR_PIN, INPUT);
	xTaskCreate(buzzerTask, "BuzzerTask", 2048, nullptr, 2, nullptr);

	prefs.begin("pillbox", true);
	ssid = prefs.getString("s", "");
	pass = prefs.getString("p", "");
	chatId = prefs.getString("c", "");
	alarm1 = prefs.getString("t1", "08:00");
	alarm2 = prefs.getString("t2", "20:00");
	prefs.end();

	if (ssid == "")
	{
		WiFi.softAP("PillBox_Setup");
		dnsServer.start(53, "*", WiFi.softAPIP());
		server.on("/", []() {
			server.send(200, "text/html", INDEX_HTML);
		});
		server.on("/save", handleSave);
		server.onNotFound([]() {
			server.sendHeader("Location", "/", true);
			server.send(302, "text/plain", "");
		});
		server.begin();
	}
	else
	{
		WiFi.begin(ssid.c_str(), pass.c_str());
		int a = 0;
		while (WiFiClass::status() != WL_CONNECTED && a < 40)
		{
			delay(500);
			Serial.print(".");
			a++;
		}
		if (WiFiClass::status() == WL_CONNECTED)
		{
			configTime(8 * 3600, 0, "pool.ntp.org");
			int t_cnt = 0;
			while (time(nullptr) < 100000 && t_cnt < 20)
			{
				delay(500);
				t_cnt++;
			}
			sendMsg("Таблетница в сети! 🟢");
		}
		else
		{
			prefs.begin("pillbox", false);
			prefs.remove("s");
			prefs.end();
			ESP.restart();
		}
	}
}

void loop()
{
	if (WiFiClass::getMode() == WIFI_AP)
	{
		dnsServer.processNextRequest();
		server.handleClient();
	}
	else
	{
		if (millis() - lastBotCheck > 7000)
		{
			lastBotCheck = millis();
			checkBot();
		}
		static unsigned long lastT = 0;
		if (millis() - lastT > 1000)
		{
			lastT = millis();
			const time_t now = time(nullptr);
			const tm *ti = localtime(&now);
			if (ti->tm_year > 70)
			{
				char buf[6];
				sprintf(buf, "%02d:%02d", ti->tm_hour, ti->tm_min);
				const auto cur = String(buf);
				if ((cur == alarm1 || cur == alarm2) && cur != lastTriggeredMinute)
				{
					if (digitalRead(LID_SENSOR_PIN) == HIGH)
					{
						isAlerting = true;
						lastTriggeredMinute = cur;
						sendMsg("🔔 ВРЕМЯ ПИТЬ ТАБЛЕТКИ!");
					}
				}
			}
		}

		if (isAlerting && digitalRead(LID_SENSOR_PIN) == LOW)
		{
			isAlerting = false; 
			digitalWrite(BUZZER_PIN, LOW);

			Serial.println("Крышка открыта - мгновенный сброс");
			vTaskDelay(100 / portTICK_PERIOD_MS);

			sendMsg("✅ Таблетки приняты."); // только теперь шлем сообщение
		}
	}
}
