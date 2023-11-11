/*
Este é um servidor web de streaming imples implementado para AI-Thinker ESP32-CAM
   e módulos ESP-EYE.
   Isso foi testado e pode suportar até 10 clientes de streaming ligados simultaneamente.
   O streaming simultâneo é implementado com tarefas do FreeRTOS.

   Inspirado e baseado:(https://www.hackster.io/anatoli-arkhipenko/multi-client-mjpeg-streaming-from-esp32-47768f/)


*/

// ESP32 tem dois núcleos: núcleo APPlation e núcleo PROcess (aquele que executa a pilha ESP32 SDK)
#define APP_CPU 1
#define PRO_CPU 0

#include "src/OV2640.h"
#include <WiFi.h>
#include <WebServer.h>
#include <WiFiClient.h>

#include <esp_bt.h>
#include <esp_wifi.h>
#include <esp_sleep.h>
#include <driver/rtc_io.h>


#define CAMERA_MODEL_AI_THINKER

#include "camera_pins.h"

// Incluir credenciais wifi.
#define SSID1 " "
#define PWD1 " "

OV2640 cam;

WebServer server(80);

// Streaming is implemented with 3 tasks:
TaskHandle_t tMjpeg;   // ligações do cliente com o servidor web
TaskHandle_t tCam;     // obtenção de molduras de fotos da cÂmara e o armazenamento localmente
TaskHandle_t tStream;  // transmite quadros para todos os clientes ligados

// Para evitar buffer de streaming à medida que é substituído pelo próximo quadro
SemaphoreHandle_t frameSync = NULL;

// A fila armazena clientes atualmente ligadois para os quais estamos a transmitir
QueueHandle_t streamingClients;

const int FPS = 14;

// Solicitações dos clientes a cada 50ms
const int WSINTERVAL = 100;


// ======== Tarefa de ligação do servidor ==========================
void mjpegCB(void* pvParameters) {
  TickType_t xLastWakeTime;
  const TickType_t xFrequency = pdMS_TO_TICKS(WSINTERVAL);

  // Cria semáforo de sincronização de quadros e inicializando-o
  frameSync = xSemaphoreCreateBinary();
  xSemaphoreGive(frameSync);

  // Cria uma fila para verificar todos os clientes ligados
  streamingClients = xQueueCreate(10, sizeof(WiFiClient*));

  //=== Secção de configuração ==================

  //  Criar tarefa RTOS para capturar quadros da câmara
  xTaskCreatePinnedToCore(
    camCB,     // callback
    "cam",     // name
    4096,      // stacj size
    NULL,      // parameters
    2,         // priority
    &tCam,     // RTOS task handle
    APP_CPU);  // core

  //  Criar tarefa para enviar o stream para todos os clientes ligados
  xTaskCreatePinnedToCore(
    streamCB,
    "strmCB",
    4 * 1024,
    NULL,  //(void*) handler,
    2,
    &tStream,
    APP_CPU);

  //  Regista rotina de manipulação de servidor web
  server.on("/stream", HTTP_GET, handleJPGSstream);
  server.on("/jpg", HTTP_GET, handleJPG);
  server.onNotFound(handleNotFound);

  //  Iniciar servidor web
  server.begin();

  //=== secção do loop()  ===================
  xLastWakeTime = xTaskGetTickCount();
  for (;;) {
    server.handleClient();

    //  Após cada solicitação de tratamento do cliente do servidor, deixamos outras tarefas serem executadas e depois entra em pausa
    taskYIELD();
    vTaskDelayUntil(&xLastWakeTime, xFrequency);
  }
}


// Variáveis usadas
volatile size_t camSize;  // tamanho do quadro atual, byte
volatile char* camBuf;    // ponteiro para o quadro atual


// ==== Tarefa RTOS para capturar quadros da câmara =========================
void camCB(void* pvParameters) {

  TickType_t xLastWakeTime;

  //  Intervalo de execução associado à taxa de quadros atualmente desejada
  const TickType_t xFrequency = pdMS_TO_TICKS(1000 / FPS);

  // Mutex para a seção crítica de mudança dos quadros ativos
  portMUX_TYPE xSemaphore = portMUX_INITIALIZER_UNLOCKED;

  //  Ponteiros para os 2 quadros, respectivos tamanhos e índice do quadro atual
  char* fbs[2] = { NULL, NULL };
  size_t fSize[2] = { 0, 0 };
  int ifb = 0;

  //=== secção do loop()  ===================
  xLastWakeTime = xTaskGetTickCount();

  for (;;) {

    //  Pega num quadro da câmara e consulta o tamanho
    cam.run();
    size_t s = cam.getSize();

    //  Se o tamanho do quadro for maior do que o alocado anteriormente - solicita 125% do espaço do quadro atual
    if (s > fSize[ifb]) {
      fSize[ifb] = s * 4 / 3;
      fbs[ifb] = allocateMemory(fbs[ifb], fSize[ifb]);
    }

    //  Copia o quadro atual para o buffer local
    char* b = (char*)cam.getfb();
    memcpy(fbs[ifb], b, s);

    //  Deixa outras tarefas serem executadas e aguarda até o final do intervalo de taxa de quadros atual (se houver algum tempo restante)
    taskYIELD();
    vTaskDelayUntil(&xLastWakeTime, xFrequency);

    //  Só alterna os frames se nenhum frame estiver a ser transmitido para um cliente
    //Espera num semáforo até que a operação do cliente seja concluída
    xSemaphoreTake(frameSync, portMAX_DELAY);

    //  Não permitir interrupções ao alternar o quadro atual
    portENTER_CRITICAL(&xSemaphore);
    camBuf = fbs[ifb];
    camSize = s;
    ifb++;
    ifb &= 1;  // isto deve produzir 1, 0, 1, 0, 1 ... sequência
    portEXIT_CRITICAL(&xSemaphore);

    //  Deixa qualquer pessoa que esteja à espera por um quadro saber que o quadro está pronto
    xSemaphoreGive(frameSync);

    //  tecnicamente só é necessário uma vez: informa a tarefa de streaming que temos pelo menos um quadro
    // e, caso existam, pode começar a enviar frames para os clientes
    xTaskNotifyGive(tStream);

    //  Deiae imediatamente outras tarefas (streaming) serem executadas
    taskYIELD();

    //  Se a tarefa de streaming foi suspensa (não há clientes ativos para transmitir)
    // não há necessidade de capturar quadros da câmara. Podemos economizar
    // suspendendo as tarefas
    if (eTaskGetState(tStream) == eSuspended) {
      vTaskSuspend(NULL);  // passar NULL significa "suspender"
    }
  }
}


// ==== Alocação de memória que aproveita PSRAM, se existir =======================
char* allocateMemory(char* aPtr, size_t aSize) {

  //  Como o buffer atual é muito pequeno, liberta-o
  if (aPtr != NULL) free(aPtr);


  size_t freeHeap = ESP.getFreeHeap();
  char* ptr = NULL;

  // Se a memória solicitada for superior a 2/3 do heap atualmente livre, tente PSRAM imediatamente
  if (aSize > freeHeap * 2 / 3) {
    if (psramFound() && ESP.getFreePsram() > aSize) {
      ptr = (char*)ps_malloc(aSize);
    }
  } else {
    //  Chega de heap livre - vamos tentar alocar RAM rápida como buffer
    ptr = (char*)malloc(aSize);

    //  Se a alocação no heap falhar, vamos dar mais uma hipótese ao PSRAM
    if (ptr == NULL && psramFound() && ESP.getFreePsram() > aSize) {
      ptr = (char*)ps_malloc(aSize);
    }
  }

  // Finalmente, se o ponteiro de memória for NULL, não conseguimos alocar nenhuma memória, e isso é uma condição terminal.
  if (ptr == NULL) {
    ESP.restart();
  }
  return ptr;
}


// ==== STREAMING ======================================================
const char HEADER[] = "HTTP/1.1 200 OK\r\n"
                      "Access-Control-Allow-Origin: *\r\n"
                      "Content-Type: multipart/x-mixed-replace; boundary=123456789000000000000987654321\r\n";
const char BOUNDARY[] = "\r\n--123456789000000000000987654321\r\n";
const char CTNTTYPE[] = "Content-Type: image/jpeg\r\nContent-Length: ";
const int hdrLen = strlen(HEADER);
const int bdrLen = strlen(BOUNDARY);
const int cntLen = strlen(CTNTTYPE);


// ==== Lidar com solicitação de ligação de clientes ===============================
void handleJPGSstream(void) {
  //  Só pode ligar 10 clientes. O limite é um padrão para ligações WiFi
  if (!uxQueueSpacesAvailable(streamingClients)) return;


  //  Criar um novo objeto WiFi Client para acompanhar
  WiFiClient* client = new WiFiClient();
  *client = server.client();

  //  Envia imediatamente a este cliente um cabeçalho
  client->write(HEADER, hdrLen);
  client->write(BOUNDARY, bdrLen);

  // Envia o cliente para a fila de streaming
  xQueueSend(streamingClients, (void*)&client, 0);

  // Ativa tarefas de streaming, caso tenham sido suspensas anteriormente
  if (eTaskGetState(tCam) == eSuspended) vTaskResume(tCam);
  if (eTaskGetState(tStream) == eSuspended) vTaskResume(tStream);
}


// ==== transmite conteúdo para todos os clientes ligados ========================
void streamCB(void* pvParameters) {
  char buf[16];
  TickType_t xLastWakeTime;
  TickType_t xFrequency;

  //  Espera até que o primeiro quadro seja capturado e haja algo para enviar para os clientes
  ulTaskNotifyTake(pdTRUE,         /* Limpa o valor da notificação antes de sair. */
                   portMAX_DELAY); /* Bloquear indefinidamente. */

  xLastWakeTime = xTaskGetTickCount();
  for (;;) {
    // Suposição padrão de que estamos a executar de acordo com o FPS
    xFrequency = pdMS_TO_TICKS(1000 / FPS);

    //  Só se preocupa em enviar alguma coisa se houver alguém ligado
    UBaseType_t activeClients = uxQueueMessagesWaiting(streamingClients);
    if (activeClients) {
      // Ajusta a fequÊncia ao número de clientes ligados
      xFrequency /= activeClients;

      //  Como estamos a enviar o mesmo quadro para todos, retira um cliente do início da fila
      WiFiClient* client;
      xQueueReceive(streamingClients, (void*)&client, 0);

      //  Verifica se este cliente ainda está ligado.

      if (!client->connected()) {
        // exclui esta referência de cliente se estiver desligado e não o coloca mais na fila.
        delete client;
      } else {

        //  OK. Este é um cliente ativamente ligado. Vamos agarrar um semáforo para evitar mudanças de quadro enquanto estão com esta frame
        xSemaphoreTake(frameSync, portMAX_DELAY);

        client->write(CTNTTYPE, cntLen);
        sprintf(buf, "%d\r\n\r\n", camSize);
        client->write(buf, strlen(buf));
        client->write((char*)camBuf, (size_t)camSize);
        client->write(BOUNDARY, bdrLen);

        // Como este cliente ainda está ligado, empurra-o até o final da fila para processamento adicional
        xQueueSend(streamingClients, (void*)&client, 0);

        // O quadro foi disponibilizado. Liberta o semáforo e deixa outras tarefas serem executadas.

        xSemaphoreGive(frameSync);
        taskYIELD();
      }
    } else {
      //  Como não há clientes ligados, não há razão para desperdiçar bateria e ficar em funcionamento
      vTaskSuspend(NULL);
    }
    //  Deixa outras tarefas serem executadas depois de atender cada cliente
    taskYIELD();
    vTaskDelayUntil(&xLastWakeTime, xFrequency);
  }
}



const char JHEADER[] = "HTTP/1.1 200 OK\r\n"
                       "Content-disposition: inline; filename=capture.jpg\r\n"
                       "Content-type: image/jpeg\r\n\r\n";
const int jhdLen = strlen(JHEADER);

// ==== Serve um quadro JPEG =============================================
void handleJPG(void) {
  WiFiClient client = server.client();

  if (!client.connected()) return;
  cam.run();
  client.write(JHEADER, jhdLen);
  client.write((char*)cam.getfb(), cam.getSize());
}


// ==== Lidar com solicitações de URL inválidas ============================================
void handleNotFound() {
  String message = "Server is running!\n\n";
  message += "URI: ";
  message += server.uri();
  message += "\nMethod: ";
  message += (server.method() == HTTP_GET) ? "GET" : "POST";
  message += "\nArguments: ";
  message += server.args();
  message += "\n";
  server.send(200, "text / plain", message);
}



// ==== Método SETUP ==================================================================
void setup() {

  // Configurar ligação serial
  Serial.begin(115200);
  delay(1000);  // wait for a second to let Serial connect


  // Configurar a câmara
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;

  config.frame_size = FRAMESIZE_VGA;
  config.jpeg_quality = 12;
  config.fb_count = 2;

#if defined(CAMERA_MODEL_ESP_EYE)
  pinMode(13, INPUT_PULLUP);
  pinMode(14, INPUT_PULLUP);
#endif

  if (cam.init(config) != ESP_OK) {
    Serial.println("Error initializing the camera");
    delay(10000);
    ESP.restart();
  }


  //  Configurar ligação WiFi
  IPAddress ip;

  WiFi.mode(WIFI_STA);
  WiFi.begin(SSID1, PWD1);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(F("."));
  }
  ip = WiFi.localIP();
  Serial.println(F("WiFi connected"));
  Serial.println("");
  Serial.print("Stream Link: http://");
  Serial.print(ip);
  Serial.println("/stream");


  // Começar a integrar a tarefa RTOS
  xTaskCreatePinnedToCore(
    mjpegCB,
    "stream",
    4 * 1024,
    NULL,
    2,
    &tMjpeg,
    APP_CPU);
}


void loop() {
  vTaskDelay(1000);
}
