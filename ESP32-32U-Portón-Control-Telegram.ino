// ============================================
// CONTROL DE PORTÓN ELÉCTRICO - FINAL
// ESP32-32U + A7608SA-H + Telegram
// @PortonSantaClarav3_bot
// Serial2 - httpEnCurso SIEMPRE se libera
// ============================================

#include "FS.h"
#include "LittleFS.h"
#include <ArduinoJson.h>

#define MODEM_TX 17
#define MODEM_RX 16
#define RELAY_PIN 4
#define LED_PIN 2

const char botToken[] = "8591157578:AAHgmsJB7rHXC9yXRgVyH8PrqZtZTKqRtyY";
String claveMaestra = "2293";
const char apn[] = "internet.itelcel.com";
const char* ARCHIVO_NUMEROS = "/numeros.txt";
const char* ARCHIVO_ADMINS = "/admins.txt";

const unsigned long DURACION_RELAY = 800;
const unsigned long DURACION_LLAMADA = 500;
const unsigned long INTERVALO_TELEGRAM = 5000;

String llamadaPendiente = "";
bool llamadaNueva = false;

SemaphoreHandle_t semaforoModem, semaforoArchivos, semaforoSistemaListo;
portMUX_TYPE spinlockRelay = portMUX_INITIALIZER_UNLOCKED;

volatile bool relayActivo = false, llamadaEnCurso = false, sistemaListo = false, modemListo = false;
volatile unsigned long tiempoEncendido = 0, tiempoLlamada = 0;

#define MAX_NUMEROS 500
#define MAX_ADMINS 20
String numerosRAM[MAX_NUMEROS];
int totalNumerosRAM = 0;
String adminsRAM[MAX_ADMINS];
int totalAdminsRAM = 0;

String extraerDato(String data, int comillas) {
  int actual = 0, inicio = -1;
  for (int i = 0; i < data.length(); i++) {
    if (data[i] == '"') { actual++; if (actual == 1) inicio = i + 1; if (actual == 2) return data.substring(inicio, i); }
  }
  return "";
}
void limpiarBufferModem() { while (Serial2.available()) Serial2.read(); }

void cargarAdminsEnRAM() {
  xSemaphoreTake(semaforoArchivos, portMAX_DELAY); totalAdminsRAM = 0;
  File file = LittleFS.open(ARCHIVO_ADMINS, "r");
  if (file) { while (file.available() && totalAdminsRAM < MAX_ADMINS) { String l = file.readStringUntil('\n'); l.trim(); if (l.length() >= 5) adminsRAM[totalAdminsRAM++] = l; } file.close(); }
  if (totalAdminsRAM == 0) { adminsRAM[0] = "5405162685"; totalAdminsRAM = 1; File f = LittleFS.open(ARCHIVO_ADMINS, "w"); if (f) { f.println("5405162685"); f.close(); } }
  xSemaphoreGive(semaforoArchivos);
}
bool esAdmin(String id) { for (int i = 0; i < totalAdminsRAM; i++) { if (adminsRAM[i] == id) return true; } return false; }
void agregarAdmin(String id) { xSemaphoreTake(semaforoArchivos, portMAX_DELAY); if (!esAdmin(id) && totalAdminsRAM < MAX_ADMINS) { adminsRAM[totalAdminsRAM++] = id; File f = LittleFS.open(ARCHIVO_ADMINS, "a"); if (f) { f.println(id); f.close(); } } xSemaphoreGive(semaforoArchivos); }
void eliminarAdmin(String id) { xSemaphoreTake(semaforoArchivos, portMAX_DELAY); if (totalAdminsRAM <= 1) { xSemaphoreGive(semaforoArchivos); return; } for (int i = 0; i < totalAdminsRAM; i++) { if (adminsRAM[i] == id) { for (int j = i; j < totalAdminsRAM - 1; j++) adminsRAM[j] = adminsRAM[j + 1]; totalAdminsRAM--; break; } } File f = LittleFS.open(ARCHIVO_ADMINS, "r"), t = LittleFS.open("/temp.txt", "w"); if (f && t) { while (f.available()) { String l = f.readStringUntil('\n'); l.trim(); if (l != id && l.length() >= 5) t.println(l); } f.close(); t.close(); LittleFS.remove(ARCHIVO_ADMINS); LittleFS.rename("/temp.txt", ARCHIVO_ADMINS); } xSemaphoreGive(semaforoArchivos); }

void cargarNumerosEnRAM() { xSemaphoreTake(semaforoArchivos, portMAX_DELAY); totalNumerosRAM = 0; File f = LittleFS.open(ARCHIVO_NUMEROS, "r"); if (f) { while (f.available() && totalNumerosRAM < MAX_NUMEROS) { String l = f.readStringUntil('\n'); l.trim(); if (l.length() >= 10) numerosRAM[totalNumerosRAM++] = l; } f.close(); } xSemaphoreGive(semaforoArchivos); }
bool existeNumero(String n) { String u = ""; for (int i = 0; i < n.length(); i++) { if (isDigit(n.charAt(i))) u += n.charAt(i); } if (u.length() >= 10) u = u.substring(u.length()-10); for (int i = 0; i < totalNumerosRAM; i++) { if (numerosRAM[i] == u) return true; } return false; }
void agregarNumero(String n) { xSemaphoreTake(semaforoArchivos, portMAX_DELAY); if (!existeNumero(n) && totalNumerosRAM < MAX_NUMEROS) { numerosRAM[totalNumerosRAM++] = n; File f = LittleFS.open(ARCHIVO_NUMEROS, "a"); if (f) { f.println(n); f.close(); } } xSemaphoreGive(semaforoArchivos); }
void eliminarNumero(String n) { xSemaphoreTake(semaforoArchivos, portMAX_DELAY); for (int i = 0; i < totalNumerosRAM; i++) { if (numerosRAM[i] == n) { for (int j = i; j < totalNumerosRAM - 1; j++) numerosRAM[j] = numerosRAM[j + 1]; totalNumerosRAM--; break; } } File f = LittleFS.open(ARCHIVO_NUMEROS, "r"), t = LittleFS.open("/temp.txt", "w"); if (f && t) { while (f.available()) { String l = f.readStringUntil('\n'); l.trim(); if (l != n && l.length() >= 10) t.println(l); } f.close(); t.close(); LittleFS.remove(ARCHIVO_NUMEROS); LittleFS.rename("/temp.txt", ARCHIVO_NUMEROS); } xSemaphoreGive(semaforoArchivos); }

void inicializarModem() {
  Serial2.println("AT+CGDCONT=1,\"IP\",\"" + String(apn) + "\""); delay(500); limpiarBufferModem();
  Serial2.println("AT+CGACT=1,1"); delay(5000); limpiarBufferModem();
  Serial2.println("AT+HTTPINIT"); delay(3000); limpiarBufferModem();
  Serial2.println("AT+CLIP=1"); delay(200); limpiarBufferModem();
  modemListo = true;
}
void colgarLlamada() { Serial2.println("ATH"); delay(200); Serial2.println("AT+CHUP"); delay(100); limpiarBufferModem(); llamadaEnCurso = false; digitalWrite(LED_PIN, LOW); }

// ============================================
// FUNCIÓN - enviarMensajeTelegram
// El semáforo ya está tomado desde afuera
// ============================================
void enviarMensajeTelegram(String chat_id, String texto) {
  limpiarBufferModem();
  
  Serial2.println("AT+HTTPPARA=\"URL\",\"https://api.telegram.org/bot" + String(botToken) + "/sendMessage?chat_id=" + chat_id + "&text=" + texto + "&parse_mode=Markdown\"");
  delay(500);
  
  Serial2.println("AT+HTTPACTION=0"); 
  delay(4000);
  
  // Leer respuesta del modem
  String respuesta = "";
  unsigned long ini = millis();
  while (millis() - ini < 2000) { 
    while (Serial2.available()) {
      respuesta += (char)Serial2.read();
    }
    delay(10); 
  }
  
  limpiarBufferModem();
  delay(1500);  // Delay entre mensajes
}

// ============================================
// FUNCIÓN - verificarTelegram
// ============================================
void verificarTelegram() {
  if (!modemListo) return;
  
  static String lastUpdateId = "0";
  
  limpiarBufferModem();
  
  Serial2.println("AT+HTTPPARA=\"URL\",\"https://api.telegram.org/bot" + String(botToken) + "/getUpdates?offset=" + lastUpdateId + "&limit=1&timeout=0\"");
  delay(300); 
  while (Serial2.available()) Serial2.read();
  
  Serial2.println("AT+HTTPACTION=0"); 
  delay(3000);
  
  String r = ""; 
  unsigned long ini = millis();
  while (millis() - ini < 1500) { 
    while (Serial2.available()) r += (char)Serial2.read(); 
    delay(10); 
  }

  if (r.indexOf("200") != -1) {
    Serial2.println("AT+HTTPREAD=0,1000"); 
    delay(300);
    
    String d = ""; 
    ini = millis();
    while (millis() - ini < 1500) { 
      while (Serial2.available()) d += (char)Serial2.read(); 
      delay(10); 
    }

    if (d.indexOf("\"message\"") != -1) {
      int ij = d.indexOf("{"); 
      String json = d.substring(ij);
      int ll = 0, f = -1; 
      for (int i = 0; i < json.length(); i++) { 
        if (json[i] == '{') ll++; 
        else if (json[i] == '}') { 
          ll--; 
          if (ll == 0) { f = i; break; } 
        } 
      }
      if (f > 0) json = json.substring(0, f + 1);
      
      StaticJsonDocument<1500> doc;
      if (!deserializeJson(doc, json)) {
        JsonArray results = doc["result"].as<JsonArray>();
        if (results.size() > 0) {
          for (JsonObject result : results) {
            lastUpdateId = String(result["update_id"].as<long>() + 1);
            if (!result["message"].containsKey("text")) continue;
            
            String chat_id = result["message"]["chat"]["id"].as<String>();
            String texto = result["message"]["text"].as<String>(); 
            texto.trim();
            Serial.println("📱 " + chat_id + ": " + texto);

            if (texto == "/start" || texto == "Hola" || texto == "hola") {
              enviarMensajeTelegram(chat_id, "🚪+*Control+de+Porton*"); 
              enviarMensajeTelegram(chat_id, "📱+*Agregar:*%0A`[clave] #ADD:5512345678`"); 
              enviarMensajeTelegram(chat_id, "🗑+*Eliminar:*%0A`[clave] #DEL:5512345678`"); 
              enviarMensajeTelegram(chat_id, "📋+*Lista:*%0A`[clave] #LISTA`"); 
              if (esAdmin(chat_id)) { 
                enviarMensajeTelegram(chat_id, "👑+*Admin:*%0A`[clave] #ADDADMIN:chatid`%0A`[clave] #DELADMIN:chatid`%0A`[clave] #LISTAADMINS`"); 
              }
            }
            else if (texto.startsWith(claveMaestra + " ") && esAdmin(chat_id)) {
              String orden = texto.substring(claveMaestra.length() + 1); 
              orden.trim();
              
              if (orden.startsWith("#ADDADMIN:")) { 
                String id = orden.substring(10); 
                id.trim(); 
                agregarAdmin(id); 
                enviarMensajeTelegram(chat_id, "✅+*Admin+agregado:*+`"+id+"`"); 
              }
              else if (orden.startsWith("#DELADMIN:")) { 
                String id = orden.substring(10); 
                id.trim(); 
                if (id != chat_id) { 
                  eliminarAdmin(id); 
                  enviarMensajeTelegram(chat_id, "✅+*Admin+eliminado:*+`"+id+"`"); 
                } else {
                  enviarMensajeTelegram(chat_id, "❌+No+puedes+eliminarte"); 
                }
              }
              else if (orden.startsWith("#LISTAADMINS")) { 
                String l = "👑+*Admins:*%0A"; 
                for (int i = 0; i < totalAdminsRAM; i++) {
                  l += String(i+1)+".+`"+adminsRAM[i]+"`%0A"; 
                }
                enviarMensajeTelegram(chat_id, l); 
              }
              else if (orden.startsWith("#ADD:")) { 
                String n = ""; 
                for (int k = 5; k < orden.length(); k++) { 
                  if (isDigit(orden.charAt(k))) n += orden.charAt(k); 
                } 
                if (n.length() >= 10) { 
                  n = n.substring(n.length()-10); 
                  agregarNumero(n); 
                  enviarMensajeTelegram(chat_id, "✅+*Agregado*%0A📱+`"+n+"`"); 
                } 
              }
              else if (orden.startsWith("#DEL:")) { 
                String n = ""; 
                for (int k = 5; k < orden.length(); k++) { 
                  if (isDigit(orden.charAt(k))) n += orden.charAt(k); 
                } 
                if (n.length() >= 10) { 
                  n = n.substring(n.length()-10); 
                  if (existeNumero(n)) { 
                    eliminarNumero(n); 
                    enviarMensajeTelegram(chat_id, "✅+*Eliminado*%0A📱+`"+n+"`"); 
                  } else {
                    enviarMensajeTelegram(chat_id, "❌+No+encontrado"); 
                  }
                } 
              }
              else if (orden.startsWith("#LISTA")) { 
                if (totalNumerosRAM == 0) {
                  enviarMensajeTelegram(chat_id, "📱+*Sin+usuarios*"); 
                } else { 
                  String l = "📱+*Usuarios:*%0A"; 
                  for (int i = 0; i < totalNumerosRAM; i++) {
                    l += String(i+1)+".+`"+numerosRAM[i]+"`%0A"; 
                  }
                  l += "✅+Total:+"+String(totalNumerosRAM); 
                  enviarMensajeTelegram(chat_id, l); 
                } 
              }
            }
            else if (texto.startsWith(claveMaestra + " ")) { 
              enviarMensajeTelegram(chat_id, "🔒+No+eres+admin"); 
            }
            
            return;
          }
        }
      }
    }
  }

  limpiarBufferModem();
}

void tareaLlamadas(void* parameter) {
  Serial.println("📞 [Core 0] Escuchando"); 
  xSemaphoreTake(semaforoSistemaListo, portMAX_DELAY);
  
  while (1) {
    static unsigned long hb = 0; 
    if (millis() - hb > 5000) { 
      Serial.print("💓 [Core 0]"); 
      Serial.println(); 
      hb = millis(); 
    }
    
    if (Serial2.available()) {
      String r = Serial2.readString();
      
      if (!llamadaEnCurso && (r.indexOf("+CLIP:") != -1 || r.indexOf("RING") != -1)) {
        String n = extraerDato(r, 1); 
        if (n.length() < 10) { 
          String l = ""; 
          for (int i = 0; i < r.length(); i++) { 
            if (isDigit(r.charAt(i))) l += r.charAt(i); 
          } 
          if (l.length() >= 10) n = l.substring(l.length()-10); 
        }
        
        String u = ""; 
        for (int i = 0; i < n.length(); i++) { 
          if (isDigit(n.charAt(i))) u += n.charAt(i); 
        } 
        if (u.length() >= 10) u = u.substring(u.length()-10);
        
        if (u.length() == 10) { 
          Serial.println("📞 " + u); 
          llamadaPendiente = u; 
          llamadaNueva = true; 
          xSemaphoreTake(semaforoModem, portMAX_DELAY); 
          limpiarBufferModem(); 
          Serial2.println("ATA"); 
          llamadaEnCurso = true; 
          tiempoLlamada = millis(); 
          xSemaphoreGive(semaforoModem); 
          digitalWrite(LED_PIN, HIGH); 
        }
      }
      
      if (r.indexOf("NO CARRIER") != -1 && llamadaEnCurso) { 
        llamadaEnCurso = false; 
        digitalWrite(LED_PIN, LOW); 
        portENTER_CRITICAL(&spinlockRelay); 
        if (relayActivo) { 
          digitalWrite(RELAY_PIN, LOW); 
          relayActivo = false; 
        } 
        portEXIT_CRITICAL(&spinlockRelay); 
      }
    }
    
    if (llamadaEnCurso && (millis() - tiempoLlamada >= DURACION_LLAMADA)) { 
      xSemaphoreTake(semaforoModem, portMAX_DELAY); 
      colgarLlamada(); 
      xSemaphoreGive(semaforoModem); 
      portENTER_CRITICAL(&spinlockRelay); 
      if (relayActivo) { 
        digitalWrite(RELAY_PIN, LOW); 
        relayActivo = false; 
      } 
      portEXIT_CRITICAL(&spinlockRelay); 
    }
    
    vTaskDelay(1 / portTICK_PERIOD_MS);
  }
}

void tareaTelegram(void* parameter) {
  Serial.println("🌐 [Core 1] Iniciando..."); 
  unsigned long tuc = 0;
  
  xSemaphoreTake(semaforoModem, portMAX_DELAY); 
  inicializarModem(); 
  xSemaphoreGive(semaforoModem);
  
  sistemaListo = true; 
  xSemaphoreGive(semaforoSistemaListo);
  Serial.println("🌐 [Core 1] Listo\n");
  
  while (1) {
    static unsigned long hb = 0; 
    if (millis() - hb > 8000) { 
      Serial.println("💓 [Core 1]"); 
      hb = millis(); 
    }
    
    if (llamadaNueva) { 
      llamadaNueva = false; 
      String n = llamadaPendiente; 
      bool a = existeNumero(n); 
      Serial.println((a ? "✅ " : "⛔ ") + n); 
      if (a) { 
        portENTER_CRITICAL(&spinlockRelay); 
        digitalWrite(RELAY_PIN, HIGH); 
        relayActivo = true; 
        tiempoEncendido = millis(); 
        portEXIT_CRITICAL(&spinlockRelay); 
      } 
    }
    
    if (llamadaPendiente != "" && !llamadaEnCurso && !relayActivo) { 
      String n = llamadaPendiente; 
      bool a = existeNumero(n); 
      String msg = a ? "🟢+*Porton+abierto*%0A📱+`"+n+"`" : "🔴+*Acceso+denegado*%0A📱+`"+n+"`"; 
      
      if (xSemaphoreTake(semaforoModem, 1000) == pdTRUE) { 
        enviarMensajeTelegram("5405162685", msg); 
        xSemaphoreGive(semaforoModem); 
      }
      
      llamadaPendiente = ""; 
    }
    
    portENTER_CRITICAL(&spinlockRelay); 
    if (relayActivo && (millis() - tiempoEncendido >= DURACION_RELAY)) { 
      digitalWrite(RELAY_PIN, LOW); 
      relayActivo = false; 
    } 
    portEXIT_CRITICAL(&spinlockRelay);
    
    if (modemListo && (millis() - tuc >= INTERVALO_TELEGRAM) && !llamadaEnCurso) { 
      if (xSemaphoreTake(semaforoModem, 1000) == pdTRUE) { 
        verificarTelegram(); 
        xSemaphoreGive(semaforoModem); 
        tuc = millis(); 
      } 
    }
    
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void tareaMantenimiento(void* parameter) { 
  bool l = false; 
  unsigned long t = 0; 
  while (1) { 
    if (millis() - t > 2000) { 
      l = !l; 
      digitalWrite(LED_PIN, l ? HIGH : LOW); 
      t = millis(); 
    } 
    vTaskDelay(100 / portTICK_PERIOD_MS); 
  } 
}

void setup() {
  Serial.begin(115200); 
  pinMode(LED_PIN, OUTPUT); 
  digitalWrite(LED_PIN, HIGH); 
  pinMode(RELAY_PIN, OUTPUT); 
  digitalWrite(RELAY_PIN, LOW);
  
  Serial.println("\n╔══════════════════════════════════════╗");
  Serial.println("║   CONTROL DE PORTON ELECTRICO       ║");
  Serial.println("║   ESP32-32U + A7608SA-H             ║");
  Serial.println("╚══════════════════════════════��═══════╝\n");
  Serial.println("⏳ Esperando 10s...");
  for (int i = 10; i > 0; i--) { 
    Serial.print("   " + String(i) + "..."); 
    delay(1000); 
  }
  Serial.println("\n✅ Iniciando...\n");
  
  Serial2.begin(115200, SERIAL_8N1, MODEM_RX, MODEM_TX);
  if (!LittleFS.begin(false)) LittleFS.begin(true);
  
  semaforoModem = xSemaphoreCreateMutex(); 
  semaforoArchivos = xSemaphoreCreateMutex(); 
  semaforoSistemaListo = xSemaphoreCreateBinary();
  
  cargarAdminsEnRAM(); 
  cargarNumerosEnRAM();
  
  xTaskCreatePinnedToCore(tareaLlamadas, "C0", 4096, NULL, 3, NULL, 0);
  xTaskCreatePinnedToCore(tareaTelegram, "C1", 8192, NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(tareaMantenimiento, "M", 4096, NULL, 1, NULL, 1);
  
  Serial.println("🚀 Sistema iniciado\n");
}

void loop() { 
  vTaskDelete(NULL); 
}
