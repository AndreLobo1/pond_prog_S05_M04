#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

// Definindo os pinos dos LEDs e outros componentes
const uint8_t ledVermelho = 18;
const uint8_t ledAmarelo = 19;
const uint8_t ledVerde = 21;
const uint8_t buzzer = 22;
const uint8_t ldrPin = 34;        // LDR do ambiente
const uint8_t ldrCarroPin = 35;   // LDR para detectar carros

// Configuração Wi-Fi
const char* ssid = "Inteli.Iot";
const char* password = "@Intelix10T#";

// Configuração MQTT Ubidots
const char* mqtt_server = "industrial.api.ubidots.com";
const char* mqtt_token = "BBUS-th90PttoOcHlpGEaNfr2i4B3EC2ech"; // Token do dispositivo B
const char* client_id = "esp32_t12_g04_a"; // Identificador único para o dispositivo B

// Tópicos MQTT
const char* topic_modo_noturno = "/v1.6/devices/esp32_t12_g04_a/modo_noturno";
const char* topic_ldr = "/v1.6/devices/esp32_t12_g04_a/ldr_valor";
const char* topic_led_verde = "/v1.6/devices/esp32_t12_g04_a/led_verde"; 

WiFiClient espClient;
PubSubClient client(espClient);

bool modoNoturnoGlobal = false;
bool modoNoturnoAutomatico = false;
bool carroDetectado = false; // Estado do novo LDR
bool ledVerdeManual = false; // Novo estado para LED verde manual

// Variáveis para armazenar o tempo configurado
unsigned long tempoVermelho = 6000;
unsigned long tempoAmarelo = 2000;
unsigned long tempoVerde = 2000;

const float resistorSerie = 10000; // Resistor em série com o LDR em ohms

class Semaforo {
  private:
    uint8_t estado;
    unsigned long tempoAnterior;

  public:
    Semaforo() : estado(0), tempoAnterior(0) {}

    void apagarTodos() {
      digitalWrite(ledVermelho, LOW);
      digitalWrite(ledAmarelo, LOW);
      digitalWrite(ledVerde, LOW);
      digitalWrite(buzzer, LOW);
    }

    void iniciarModoDiurno() {
      unsigned long tempoAtual = millis();

      switch (estado) {
        case 0:  // Vermelho
          digitalWrite(ledVermelho, HIGH);
          if (tempoAtual - tempoAnterior >= tempoVermelho) {
            digitalWrite(ledVermelho, LOW);
            estado = 1;
            tempoAnterior = tempoAtual;
          }
          break;

        case 1:  // Amarelo
          digitalWrite(ledAmarelo, HIGH);
          if (tempoAtual - tempoAnterior >= tempoAmarelo) {
            digitalWrite(ledAmarelo, LOW);
            estado = 2;
            tempoAnterior = tempoAtual;
          }
          break;

        case 2:  // Verde
          digitalWrite(ledVerde, HIGH);
          if (tempoAtual - tempoAnterior >= tempoVerde) {
            digitalWrite(ledVerde, LOW);
            estado = 3;
            tempoAnterior = tempoAtual;
          }
          break;

        case 3:  // Amarelo piscando
          if (tempoAtual - tempoAnterior >= tempoAmarelo) {
            digitalWrite(ledAmarelo, LOW);
            estado = 0;
            tempoAnterior = tempoAtual;
          } else {
            digitalWrite(ledAmarelo, HIGH);
          }
          break;
      }
    }

    void iniciarModoNoturno() {
      static bool estadoAmarelo = false; // Armazena o estado atual do LED (ligado ou desligado)
      unsigned long tempoAtual = millis();

      // Defina tempos diferentes para os estados ligado e desligado
      unsigned long tempoLigado = 500; // Tempo que o LED ficará ligado (500 ms)
      unsigned long tempoDesligado = 1000; // Tempo que o LED ficará desligado (1000 ms)

      if (estadoAmarelo && (tempoAtual - tempoAnterior >= tempoLigado)) {
        // Se o LED está ligado e já passou o tempoLigado, desligue-o
        digitalWrite(ledAmarelo, LOW);
        digitalWrite(buzzer, LOW);
        estadoAmarelo = false;
        tempoAnterior = tempoAtual;
      } else if (!estadoAmarelo && (tempoAtual - tempoAnterior >= tempoDesligado)) {
        // Se o LED está desligado e já passou o tempoDesligado, ligue-o
        digitalWrite(ledAmarelo, HIGH);
        digitalWrite(buzzer, HIGH);
        estadoAmarelo = true;
        tempoAnterior = tempoAtual;
      }
    }

    void manterVerde() {
      apagarTodos();
      digitalWrite(ledVerde, HIGH);
    }
};

class SensorLDR {
  private:
    uint16_t valorLDR;
    uint16_t limiteBaixo;
    uint16_t limiteAlto;

  public:
    SensorLDR() : valorLDR(0), limiteBaixo(0), limiteAlto(0) {}

    void inicializar(uint8_t pin) {
      valorLDR = analogRead(pin);
      limiteBaixo = valorLDR * 0.3;
      limiteAlto = valorLDR * 0.7;
      Serial.print("LDR no pino ");
      Serial.print(pin);
      Serial.print(" - Limite LDR Baixo definido em: ");
      Serial.println(limiteBaixo);
      Serial.print("Limite LDR Alto definido em: ");
      Serial.println(limiteAlto);
    }

    int lerValorLDR(uint8_t pin) {
      valorLDR = analogRead(pin);
      return valorLDR;
    }

    float converterLDRParaLux(int valorADC) {
      float resistenciaLDR = resistorSerie * ((4095.0 / valorADC) - 1);
      float lux = 500 / (resistenciaLDR / 1000);
      return lux;
    }

    bool verificarLuminosidade(uint8_t pin) {
      valorLDR = lerValorLDR(pin);
      return valorLDR < limiteBaixo;
    }
};

Semaforo semaforo;
SensorLDR sensorAmbiente;
SensorLDR sensorCarro;

void callback(char* topic, byte* payload, unsigned int length) {
  payload[length] = '\0'; // Garante que a string esteja terminada
  Serial.print("Mensagem recebida no tópico: ");
  Serial.println(topic);

  if (String(topic) == topic_modo_noturno) {
    DynamicJsonDocument doc(1024);
    DeserializationError error = deserializeJson(doc, payload);

    if (error) {
      Serial.print("Erro ao analisar JSON: ");
      Serial.println(error.c_str());
      return;
    }

    float valorRecebido = doc["value"];
    Serial.print("Valor recebido para modo_noturno: ");
    Serial.println(valorRecebido);

    if (valorRecebido == 1) {
      modoNoturnoGlobal = true;
      modoNoturnoAutomatico = false; // Desativa o modo automático
      semaforo.apagarTodos(); // Garante que todos os LEDs sejam apagados
      Serial.println("Modo noturno ativado via MQTT");
    } else if (valorRecebido == 0) {
      modoNoturnoGlobal = false;
      semaforo.apagarTodos();
      Serial.println("Modo noturno desativado via MQTT");
    }
  }

  if (String(topic) == topic_led_verde) {
    DynamicJsonDocument doc(1024);
    DeserializationError error = deserializeJson(doc, payload);

    if (error) {
      Serial.print("Erro ao analisar JSON: ");
      Serial.println(error.c_str());
      return;
    }

    float valorRecebido = doc["value"];
    Serial.print("Valor recebido para led_verde: ");
    Serial.println(valorRecebido);

    if (valorRecebido == 1) {
      ledVerdeManual = true;
      digitalWrite(ledVerde, HIGH);
      Serial.println("LED verde ativado via MQTT");
    } else if (valorRecebido == 0) {
      ledVerdeManual = false;
      digitalWrite(ledVerde, LOW);
      Serial.println("LED verde desativado via MQTT");
    }
  }
}

void setup_wifi() {
  delay(10);
  Serial.println();
  Serial.print("Conectando a ");
  Serial.println(ssid);

  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println();
  Serial.println("WiFi conectado");
  Serial.print("Endereço IP: ");
  Serial.println(WiFi.localIP());
}

void reconnect() {
  while (!client.connected()) {
    Serial.print("Tentando conexão MQTT...");
    if (client.connect(client_id, mqtt_token, "")) {
      Serial.println("Conectado ao MQTT");
      client.subscribe(topic_modo_noturno);
      client.subscribe(topic_led_verde); // Inscrição no tópico do LED verde
    } else {
      Serial.print("Falha, rc=");
      Serial.print(client.state());
      Serial.println(" Tentando novamente em 5 segundos");
      delay(5000);
    }
  }
}

void setup() {
  pinMode(ledVermelho, OUTPUT);
  pinMode(ledAmarelo, OUTPUT);
  pinMode(ledVerde, OUTPUT);
  pinMode(buzzer, OUTPUT);

  sensorAmbiente.inicializar(ldrPin);
  sensorCarro.inicializar(ldrCarroPin);

  Serial.begin(115200);
  setup_wifi();

  client.setServer(mqtt_server, 1883);
  client.setCallback(callback);
}

void loop() {
  if (!client.connected()) {
    reconnect();
  }
  client.loop();

  int ldrValue = sensorAmbiente.lerValorLDR(ldrPin);
  float lux = sensorAmbiente.converterLDRParaLux(ldrValue);

  DynamicJsonDocument doc(1024);
  doc["value"] = lux;
  String ldrPayload;
  serializeJson(doc, ldrPayload);
  client.publish(topic_ldr, ldrPayload.c_str());

  bool ambienteEscuro = sensorAmbiente.verificarLuminosidade(ldrPin);
  bool carroPresente = sensorCarro.verificarLuminosidade(ldrCarroPin);

  if (!modoNoturnoAutomatico && modoNoturnoGlobal) {
    semaforo.iniciarModoNoturno();
    return; // Garante que o modo noturno manual prevaleça
  }

  if (modoNoturnoGlobal && !ambienteEscuro) {
    modoNoturnoGlobal = false;
    semaforo.apagarTodos();
    Serial.println("Modo noturno desativado, luminosidade restaurada.");
  }

  if (ledVerdeManual) {
    return; // Se o LED verde manual está ativo, nada mais deve ser feito
  }

  if (modoNoturnoGlobal) {
    semaforo.iniciarModoNoturno();
  } else if (carroPresente) {
    semaforo.manterVerde();
    carroDetectado = true;
    Serial.println("Carro detectado, semáforo mantido verde.");
  } else if (!carroPresente && carroDetectado) {
    carroDetectado = false;
    semaforo.apagarTodos();
    semaforo.iniciarModoDiurno();
    Serial.println("Carro removido, retornando ao modo diurno.");
  } else if (ambienteEscuro) {
    modoNoturnoAutomatico = true;
    modoNoturnoGlobal = true;
    semaforo.apagarTodos();
    semaforo.iniciarModoNoturno();
    Serial.println("Ambiente escuro, modo noturno ativado automaticamente.");
  } else {
    modoNoturnoAutomatico = false;
    semaforo.iniciarModoDiurno();
    Serial.println("Modo diurno ativo.");
  }

  delay(100);
}
