#include <Arduino.h>
#include <ArduinoJson.h>
#include <DNSServer.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <sys/time.h>


const String BOT_TOKEN = "";
const String G_SCRIPT_URL = "";
constexpr int LID_SENSOR_PIN = 4;
constexpr int BUZZER_PIN = 0;

String ssid, pass, chatId, alarm1, alarm2;
String lastTriggeredMinute = "";
volatile bool isAlerting = false;
long lastUpdateId = 0; // ID последнего обработанного сообщения из Telegram

Preferences prefs; // объект для работы с энергонезависимой памятью (Flash)
DNSServer dnsServer; // DNS-сервер для перехвата запросов (Captive Portal)
WebServer server(80); // локальный веб-сервер для настройки таблетницы по Wi-Fi
QueueHandle_t msgQueue; // очередь для безопасной передачи сообщений между задачами

// =======================  =======================

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
				String lid = (digitalRead(LID_SENSOR_PIN) == HIGH) ? "ЗАКРЫТА" : "ОТКРЫТА";
				sendMsg(
					"📊 СТАТУС:\n🕒 Время: " + String(b) + "\n🌅 Утро: " + alarm1 + "\n🌌 Вечер: " + alarm2 +
					"\n📦 Крышка: " + lid
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
<label>Утро:</label><input type="time" id="t1" value="08:00"><label>Вечер:</label><input type="time" id="t2" value="20:00">
<button onclick="save()">СОХРАНИТЬ</button></div>
<script>function save(){
const q=`s=${document.getElementById('s').value}&p=${document.getElementById('p').value}&c=${document.getElementById('c').value}&t1=${document.getElementById('t1').value}&t2=${document.getElementById('t2').value}`;
fetch('/save?'+q).then(()=>alert("Настройки сохранены! Перезагрузка..."));
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

	server.send(200, "text/plain", "OK");
	delay(2000);
	ESP.restart(); // применяем
}

// ======================= ИНИЦИАЛИЗАЦИЯ И ОСНОВНОЙ ЦИКЛ =======================

void setup()
{
	Serial.begin(115200);
	setenv("TZ", "UTC-8", 1);
	tzset(); // часовой пояс UTC+8

	pinMode(BUZZER_PIN, OUTPUT);
	pinMode(LID_SENSOR_PIN, INPUT);

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
		// Рабочий режим (STA): подключаемся к телефону
		WiFi.begin(ssid.c_str(), pass.c_str());
		int a = 0;
		while (WiFiClass::status() != WL_CONNECTED && a < 40)
		{
			delay(500);
			a++;
		}

		if (WiFiClass::status() == WL_CONNECTED)
		{
			// Подключаемся к NTP серверу времени в интернете
			configTime(8 * 3600, 0, "pool.ntp.org");
			// Ждем успешного получения времени от сервера (чтобы год стал больше 1970)
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
			// Если не смогли подключиться за 20 секунд, сбрасываем имя сети и перезагружаемся в настройки
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
		// Если мы в режиме настройки, отдаем веб-страницу пользователю
		dnsServer.processNextRequest();
		server.handleClient();
	}
	else
	{
		// Проверка совпадения времени раз в секунду
		static unsigned long lastT = 0;
		if (millis() - lastT > 1000)
		{
			lastT = millis();
			const time_t now = time(nullptr);
			const tm *ti = localtime(&now);
			if (ti->tm_year > 70)
			{
				// Если время синхронизировано с интернетом
				char buf[6];
				sprintf(buf, "%02d:%02d", ti->tm_hour, ti->tm_min);
				const auto cur = String(buf);

				// Проверяем будильники
				if ((cur == alarm1 || cur == alarm2) && cur != lastTriggeredMinute)
				{
					if (digitalRead(LID_SENSOR_PIN) == HIGH)
					{
						// Срабатываем только если крышка закрыта
						isAlerting = true;
						lastTriggeredMinute = cur;
						sendMsg("🔔 ВРЕМЯ ПИТЬ ТАБЛЕТКИ!");
					}
				}
			}
		}

		// МГНОВЕННЫЙ СБРОС СИГНАЛА ПРИ ОТКРЫТИИ КРЫШКИ
		// Если идет тревога, а датчик показал LOW (крышка открылась)
		if (isAlerting && digitalRead(LID_SENSOR_PIN) == LOW)
		{
			isAlerting = false; // сбрасываем флаг тревоги (остановит buzzerTask)
			digitalWrite(BUZZER_PIN, LOW); // на всякий случай выключаем пищалку физически
			Serial.println("Крышка открыта - сброс звука!");

			// Даем 100 мс задержки, чтобы задача buzzerTask успела увидеть изменения
			// и завершила работу, пока Wi-Fi модуль не занял процессор отправкой сообщения
			vTaskDelay(100 / portTICK_PERIOD_MS);

			sendMsg("✅ Таблетки приняты."); // кладем сообщение в очередь на отправку
		}
	}
}
