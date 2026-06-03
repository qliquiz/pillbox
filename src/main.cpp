#include <Arduino.h>
#include <ArduinoJson.h>
#include <DNSServer.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <RtcDS1302.h>
#include <ThreeWire.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>


const String BOT_TOKEN = "";
const String G_SCRIPT_URL = "";

constexpr int LID_SENSOR_PIN = 0;
constexpr int RTC_RST_PIN = 1;
constexpr int RTC_DAT_PIN = 3;
constexpr int RTC_CLK_PIN = 4;
constexpr int BUZZER_PIN = 5;
constexpr unsigned long WIFI_RETRY_INTERVAL_MS = 5UL * 60UL * 1000UL;
constexpr unsigned long WIFI_RETRY_TIMEOUT_MS = 15000UL;

String ssid, pass, chatId, alarm1, alarm2;
String lastTriggeredMinute = "";
bool wifiReconnectInProgress = false;
volatile bool isAlerting = false;
unsigned long lastWiFiRetry = 0;
unsigned long wifiRetryStartedAt = 0;
long lastUpdateId = 0; // ID последнего обработанного сообщения из Telegram

ThreeWire rtcWire(RTC_DAT_PIN, RTC_CLK_PIN, RTC_RST_PIN); // DS1302: DAT/IO, CLK/SCLK, RST/CE
RtcDS1302<ThreeWire> rtc(rtcWire);
Preferences prefs; // объект для работы с энергонезависимой памятью (Flash)
DNSServer dnsServer; // DNS-сервер для перехвата запросов (Captive Portal)
WebServer server(80); // локальный веб-сервер для настройки таблетницы по Wi-Fi
QueueHandle_t msgQueue; // очередь для безопасной передачи сообщений между задачами

// ======================= ВРЕМЯ И ДАТЧИКИ =======================

bool isLidClosed()
{
	// Кнопка крышки замыкает GPIO0 на GND, поэтому используем INPUT_PULLUP.
	return digitalRead(LID_SENSOR_PIN) == LOW;
}

String twoDigits(const uint8_t value)
{
	if (value < 10) return "0" + String(value);
	return String(value);
}

String formatRtcTime(const RtcDateTime &dt)
{
	return twoDigits(dt.Hour()) + ":" + twoDigits(dt.Minute());
}

String formatRtcDateTime(const RtcDateTime &dt)
{
	if (!dt.IsValid() || dt.Year() < 2024) return "не настроено";

	return String(dt.Year()) + "-" + twoDigits(dt.Month()) + "-" + twoDigits(dt.Day()) + " " +
	       twoDigits(dt.Hour()) + ":" + twoDigits(dt.Minute()) + ":" + twoDigits(dt.Second());
}

bool parseDateTime(const String &value, RtcDateTime &result)
{
	if (value.length() < 16) return false;
	if (value.charAt(4) != '-' || value.charAt(7) != '-' || value.charAt(13) != ':') return false;

	const char separator = value.charAt(10);
	if (separator != 'T' && separator != ' ') return false;

	const uint16_t year = value.substring(0, 4).toInt();
	const uint8_t month = value.substring(5, 7).toInt();
	const uint8_t day = value.substring(8, 10).toInt();
	const uint8_t hour = value.substring(11, 13).toInt();
	const uint8_t minute = value.substring(14, 16).toInt();

	if (year < 2024 || month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59)
	{
		return false;
	}

	result = RtcDateTime(year, month, day, hour, minute, 0);
	return result.IsValid();
}

bool isValidAlarmTime(const String &value)
{
	if (value.length() != 5 || value.charAt(2) != ':') return false;
	const int hour = value.substring(0, 2).toInt();
	const int minute = value.substring(3, 5).toInt();
	return hour >= 0 && hour <= 23 && minute >= 0 && minute <= 59;
}

void setRtcDateTime(const RtcDateTime &dt)
{
	if (rtc.GetIsWriteProtected()) rtc.SetIsWriteProtected(false);
	if (!rtc.GetIsRunning()) rtc.SetIsRunning(true);
	rtc.SetDateTime(dt);
}

// ======================= TELEGRAM =======================

// Функция для добавления сообщения в очередь на отправку (не блокирует процессор)
void sendMsg(const String &text)
{
	// Выделяем память под строку и копируем её, чтобы данные не пропали, пока задача их не заберет
	char *msg = strdup(text.c_str());
	// Пытаемся отправить указатель на строку в очередь без ожидания (0 миллисекунд)
	if (xQueueSend(msgQueue, &msg, 0) != pdPASS)
	{
		free(msg); // если очередь переполнена, удаляем сообщение, чтобы не засорять память
	}
}

void startWiFiReconnect()
{
	if (ssid == "" || WiFiClass::getMode() == WIFI_AP) return;

	WiFiClass::mode(WIFI_STA);
	WiFi.begin(ssid.c_str(), pass.c_str());
	wifiReconnectInProgress = true;
	wifiRetryStartedAt = millis();
	lastWiFiRetry = millis();
	Serial.println("Пробуем восстановить WiFi.");
}

void serviceWiFiReconnect()
{
	if (WiFiClass::getMode() == WIFI_AP || ssid == "") return;

	if (WiFiClass::status() == WL_CONNECTED)
	{
		if (wifiReconnectInProgress)
		{
			wifiReconnectInProgress = false;
			sendMsg("Таблетница снова в сети! 🟢");
		}
		return;
	}

	if (wifiReconnectInProgress)
	{
		if (millis() - wifiRetryStartedAt > WIFI_RETRY_TIMEOUT_MS)
		{
			wifiReconnectInProgress = false;
			WiFi.disconnect(true);
			WiFiClass::mode(WIFI_OFF);
			Serial.println("WiFi не найден, остаемся в автономном режиме.");
		}
		return;
	}

	if (millis() - lastWiFiRetry > WIFI_RETRY_INTERVAL_MS)
	{
		startWiFiReconnect();
	}
}

// Функция для отправки любых запросов в Telegram через Google-прокси
String callTelegram(const String &method, const String &params)
{
	if (WiFiClass::status() != WL_CONNECTED) return "";

	WiFiClientSecure client;
	client.setInsecure(); // отключаем строгую проверку SSL-сертификатов Google (экономит память)

	HTTPClient http;
	String url = G_SCRIPT_URL + "?token=" + BOT_TOKEN + "&method=" + method + "&" + params;

	url.replace("\n", "%0A");
	url.replace(" ", "%20");

	// Настраиваем правила HTTP-клиента
	http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
	// Google всегда делает редирект, мы обязаны следовать за ним
	http.setTimeout(15000); // таймаут ожидания ответа от Google — 15 секунд

	String response = "";
	if (http.begin(client, url))
	{
		const int httpCode = http.GET();
		if (httpCode == 200) response = http.getString();
		http.end();
	}
	return response;
}

// Фоновая задача отправки сообщений
[[noreturn]] void telegramTask(void *pvParameters)
{
	char *msgText;
	for (;;)
	{
		// Ждем появления сообщения в очереди. Если очередь пуста, задача "засыпает" и не тратит ресурсы процессора
		if (xQueueReceive(msgQueue, &msgText, portMAX_DELAY) == pdPASS)
		{
			callTelegram("sendMessage", "chat_id=" + chatId + "&text=" + String(msgText));
			free(msgText); // освобождаем динамическую память, выделенную под текст
		}
	}
}

// Функция разбора входящих сообщений и команд из Telegram
void checkBot()
{
	String json = callTelegram("getUpdates", "offset=" + String(lastUpdateId + 1));
	if (json == "" || json.length() < 10) return;

	DynamicJsonDocument doc(2048); // создаем буфер для разбора JSON
	if (deserializeJson(doc, json)) return;

	const JsonArray results = doc["result"].as<JsonArray>();
	for (JsonObject result: results)
	{
		lastUpdateId = result["update_id"];

		auto text = result["message"]["text"].as<String>();
		auto fromId = result["message"]["chat"]["id"].as<String>();
		fromId.trim();
		chatId.trim();

		if (fromId == chatId)
		{
			if (text.startsWith("/t1 "))
			{
				const String newAlarm = text.substring(4, 9);
				if (isValidAlarmTime(newAlarm))
				{
					alarm1 = newAlarm;
					sendMsg("✅ Утро: " + alarm1);
				}
				else
				{
					sendMsg("⚠️ Формат: /t1 HH:MM");
				}
			}
			else if (text.startsWith("/t2 "))
			{
				const String newAlarm = text.substring(4, 9);
				if (isValidAlarmTime(newAlarm))
				{
					alarm2 = newAlarm;
					sendMsg("✅ Вечер: " + alarm2);
				}
				else
				{
					sendMsg("⚠️ Формат: /t2 HH:MM");
				}
			}
			else if (text.startsWith("/time "))
			{
				RtcDateTime newTime;
				if (parseDateTime(text.substring(6), newTime))
				{
					setRtcDateTime(newTime);
					sendMsg("✅ Время RTC: " + formatRtcDateTime(newTime));
				}
				else
				{
					sendMsg("⚠️ Формат: /time YYYY-MM-DD HH:MM");
				}
			}
			else if (text == "/status")
			{
				const RtcDateTime now = rtc.GetDateTime();
				String lid = isLidClosed() ? "ЗАКРЫТА" : "ОТКРЫТА";
				sendMsg(
					"📊 СТАТУС:\n🕒 RTC: " + formatRtcDateTime(now) + "\n🌅 Утро: " + alarm1 +
					"\n🌌 Вечер: " + alarm2 + "\n📦 Крышка: " + lid
				);
			}
			// Сохраняем новые будильники во Flash-память ESP32
			prefs.begin("pillbox", false);
			prefs.putString("t1", alarm1);
			prefs.putString("t2", alarm2);
			prefs.end();
		}
	}
}

// Фоновая задача для регулярного опроса Telegram
[[noreturn]] void botPollTask(void *pvParameters)
{
	for (;;)
	{
		if (WiFiClass::status() == WL_CONNECTED && WiFiClass::getMode() == WIFI_STA)
		{
			checkBot();
		}
		vTaskDelay(7000 / portTICK_PERIOD_MS); // спим ровно 7 секунд без задержки основного ядра
	}
}

// ======================= РАБОТА С ЖЕЛЕЗОМ =======================

// Фоновая задача для пищалки (приоритет 3)
[[noreturn]] void buzzerTask(void *pvParameters)
{
	for (;;)
	{
		if (isAlerting)
		{
			// ФАЗА ЗВУКА: пищим 300 мс, но проверяем флаг крышки каждые 10 мс для мгновенного сброса
			for (int i = 0; i < 30; i++)
			{
				if (!isAlerting) break; // если крышку открыли, мгновенно прерываем писк
				digitalWrite(BUZZER_PIN, HIGH);
				vTaskDelay(10 / portTICK_PERIOD_MS);
			}
			digitalWrite(BUZZER_PIN, LOW);

			// ФАЗА ТИШИНЫ: ждем 800 мс, также регулярно опрашивая флаг крышки
			for (int i = 0; i < 80; i++)
			{
				if (!isAlerting) break;
				vTaskDelay(10 / portTICK_PERIOD_MS);
			}
		}
		else
		{
			// Если тревоги нет, гарантируем, что пищалка выключена, и "спим"
			digitalWrite(BUZZER_PIN, LOW);
			vTaskDelay(50 / portTICK_PERIOD_MS);
		}
	}
}

// HTML-сайт настройки (хранится в Flash-памяти программ PROGMEM)
constexpr char INDEX_HTML[] PROGMEM = R"=====(
<!DOCTYPE html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1.0">
<style>body{font-family:sans-serif;background:#f4f4f9;padding:20px;display:flex;flex-direction:column;align-items:center;}
.card{background:white;padding:25px;border-radius:15px;box-shadow:0 4px 10px rgba(0,0,0,0.1);width:100%;max-width:320px;}
input{width:100%;padding:12px;margin-top:10px;border:1px solid #ddd;border-radius:8px;box-sizing:border-box;font-size:16px;}
button{width:100%;background:#4CAF50;color:white;border:none;padding:15px;border-radius:8px;margin-top:20px;width:100%;}</style></head>
<body><div class="card"><h2>PillBox Setup 💊</h2>
<input id="s" placeholder="Имя WiFi сети"><input type="password" id="p" placeholder="Пароль WiFi"><input id="c" placeholder="Chat ID">
<label>Текущее время:</label><input type="datetime-local" id="dt">
<label>Утро:</label><input type="time" id="t1" value="08:00"><label>Вечер:</label><input type="time" id="t2" value="20:00">
<button onclick="save()">СОХРАНИТЬ</button></div>
<script>
function pad(n){return String(n).padStart(2,'0')}
const d=new Date();document.getElementById('dt').value=`${d.getFullYear()}-${pad(d.getMonth()+1)}-${pad(d.getDate())}T${pad(d.getHours())}:${pad(d.getMinutes())}`;
function save(){
const q=new URLSearchParams({s:document.getElementById('s').value,p:document.getElementById('p').value,c:document.getElementById('c').value,dt:document.getElementById('dt').value,t1:document.getElementById('t1').value,t2:document.getElementById('t2').value});
fetch('/save?'+q.toString()).then(()=>alert("Настройки сохранены! Перезагрузка..."));
}</script></body></html>
)=====";

// Обработчик сохранения данных, прилетевших с веб-страницы телефона
void handleSave()
{
	prefs.begin("pillbox", false); // открываем память во Flash для записи
	prefs.putString("s", server.arg("s"));
	prefs.putString("p", server.arg("p"));
	prefs.putString("c", server.arg("c"));
	prefs.putString("t1", server.arg("t1"));
	prefs.putString("t2", server.arg("t2"));
	prefs.end();

	RtcDateTime setupTime;
	if (parseDateTime(server.arg("dt"), setupTime))
	{
		setRtcDateTime(setupTime);
	}

	server.send(200, "text/plain", "OK");
	delay(2000);
	ESP.restart(); // применяем
}

void startSetupPortal()
{
	WiFiClass::mode(WIFI_AP);
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

	Serial.print("Setup portal started: ");
	Serial.println(WiFi.softAPIP());
}

void printDebugStatus(const RtcDateTime &now)
{
	Serial.print("RTC=");
	Serial.print(formatRtcDateTime(now));
	Serial.print(" alarm1=");
	Serial.print(alarm1);
	Serial.print(" alarm2=");
	Serial.print(alarm2);
	Serial.print(" lid=");
	Serial.print(isLidClosed() ? "CLOSED" : "OPEN");
	Serial.print(" alert=");
	Serial.print(isAlerting ? "YES" : "NO");
	Serial.print(" wifi=");
	Serial.println(WiFiClass::status() == WL_CONNECTED ? "CONNECTED" : "OFFLINE");
}

void serviceAlarm()
{
	static unsigned long lastCheck = 0;
	static unsigned long lastDebug = 0;

	if (millis() - lastCheck <= 1000) return;
	lastCheck = millis();

	const RtcDateTime now = rtc.GetDateTime();
	if (millis() - lastDebug > 5000)
	{
		lastDebug = millis();
		printDebugStatus(now);
	}

	if (!now.IsValid() || now.Year() < 2024)
	{
		return;
	}

	const String cur = formatRtcTime(now);
	if ((cur == alarm1 || cur == alarm2) && cur != lastTriggeredMinute)
	{
		if (isLidClosed())
		{
			isAlerting = true;
			lastTriggeredMinute = cur;
			Serial.print("Alarm triggered at ");
			Serial.println(cur);
			sendMsg("🔔 ВРЕМЯ ПИТЬ ТАБЛЕТКИ!");
		}
		else
		{
			Serial.print("Alarm matched at ");
			Serial.print(cur);
			Serial.println(", but lid is open.");
		}
	}
}

void serviceLidReset()
{
	if (!isAlerting || isLidClosed()) return;

	isAlerting = false; // сбрасываем флаг тревоги (остановит buzzerTask)
	digitalWrite(BUZZER_PIN, LOW); // на всякий случай выключаем пищалку физически
	Serial.println("Крышка открыта - сброс звука!");

	// Даем 100 мс задержки, чтобы задача buzzerTask успела увидеть изменения
	// и завершила работу, пока Wi-Fi модуль не занял процессор отправкой сообщения
	vTaskDelay(100 / portTICK_PERIOD_MS);

	sendMsg("✅ Таблетки приняты."); // кладем сообщение в очередь на отправку
}

// ======================= ИНИЦИАЛИЗАЦИЯ И ОСНОВНОЙ ЦИКЛ =======================

void setup()
{
	Serial.begin(115200);
	delay(1000);
	Serial.println();
	Serial.println("PillBox boot");

	pinMode(BUZZER_PIN, OUTPUT);
	pinMode(LID_SENSOR_PIN, INPUT_PULLUP);

	rtc.Begin();
	if (rtc.GetIsWriteProtected())
	{
		rtc.SetIsWriteProtected(false);
	}
	if (!rtc.GetIsRunning())
	{
		rtc.SetIsRunning(true);
	}

	// Создаем очередь для Telegram-сообщений (максимум 5 указателей на строки)
	msgQueue = xQueueCreate(5, sizeof(char *));

	// ЗАПУСК ПОТОКОВ
	// buzzerTask получает приоритет 3 (высокий), чтобы звук не прерывался
	xTaskCreate(buzzerTask, "BuzzerTask", 2048, nullptr, 3, nullptr);
	// Задачи отправки и приема сообщений получают приоритет 1 (фоновый) и по 10 КБ памяти стека
	xTaskCreate(telegramTask, "TelegramTask", 10240, nullptr, 1, nullptr);
	xTaskCreate(botPollTask, "BotPollTask", 10240, nullptr, 1, nullptr);

	// Загружаем сохраненные данные из памяти Flash
	prefs.begin("pillbox", true);
	ssid = prefs.getString("s", "");
	pass = prefs.getString("p", "");
	chatId = prefs.getString("c", "");
	alarm1 = prefs.getString("t1", "08:00");
	alarm2 = prefs.getString("t2", "20:00");
	prefs.end();

	if (ssid == "")
	{
		// Режим настройки (AP): запускаем свою сеть и Captive Portal
		Serial.println("WiFi settings are empty, starting setup portal.");
		startSetupPortal();
	}
	else
	{
		// Рабочий режим (STA): подключаемся к телефону
		Serial.print("Connecting to WiFi: ");
		Serial.println(ssid);
		WiFi.begin(ssid.c_str(), pass.c_str());
		int a = 0;
		while (WiFiClass::status() != WL_CONNECTED && a < 40)
		{
			delay(500);
			a++;
		}

		if (WiFiClass::status() == WL_CONNECTED)
		{
			Serial.print("WiFi connected, IP: ");
			Serial.println(WiFi.localIP());
			sendMsg("Таблетница в сети! 🟢");
		}
		else
		{
			// Если сохраненная сеть недоступна, поднимаем портал, чтобы можно было сразу перенастроить Wi-Fi.
			WiFi.disconnect(true);
			Serial.println("WiFi недоступен, запускаем портал настройки.");
			startSetupPortal();
		}
	}
}

void loop()
{
	serviceAlarm();
	serviceLidReset();

	if (WiFiClass::getMode() == WIFI_AP)
	{
		// Если мы в режиме настройки, отдаем веб-страницу пользователю
		dnsServer.processNextRequest();
		server.handleClient();
	}
	else
	{
		serviceWiFiReconnect();
	}
}
