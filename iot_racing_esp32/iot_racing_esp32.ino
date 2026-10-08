/*
 * ============================================================
 *  MQTT IoT Racing - Squelette ESP32 (version élèves)
 *  Club CPU
 * ============================================================
 *  Carte        : ESP32 Dev Module (ESP32 classique)
 *  Bibliothèques (Gestionnaire de bibliothèques) :
 *    - PubSubClient            (Nick O'Leary)
 *    - DHT sensor library      (Adafruit)
 *    - Adafruit Unified Sensor (Adafruit, demandée par la précédente)
 *  Moniteur série : 115200 bauds
 *
 *  Ce qui est DÉJÀ fait : constantes, topics, structure non bloquante,
 *                         fonctions utilitaires, boucle principale.
 *  Ce que TU fais       : les fonctions marquées TODO.
 *
 *  Règle d'or : AUCUN delay() dans loop() ni dans tes fonctions.
 *  On chronomètre avec millis().
 * ============================================================
 */

#include <WiFi.h>
#include <PubSubClient.h>
#include "DHT.h"

// ============================================================
//  1) CONSTANTES À MODIFIER
// ============================================================

// --- Wi-Fi (2,4 GHz uniquement) ---
#define WIFI_SSID        "NOM_DU_WIFI"
#define WIFI_PASSWORD    "MOT_DE_PASSE"

// --- Broker MQTT (donné par l'animateur) ---
#define MQTT_BROKER      "192.168.1.10"
#define MQTT_PORT        1883

// --- Ton équipe : G1, G2, G3... (unique pour chaque équipe !) ---
#define TEAM_ID          "G1"

// --- Préfixe des topics (ne change que si l'animateur le demande) ---
#define TOPIC_PREFIX     "iot-racing/"

// ============================================================
//  2) BROCHES
// ============================================================
#define PIN_POT          34   // potentiomètre (ADC1, compatible Wi-Fi)
#define PIN_TRIG         25   // HC-SR04 TRIG
#define PIN_ECHO         26   // HC-SR04 ECHO (via diviseur 1k/2k !)
#define PIN_DHT           4   // DHT11 DATA
#define PIN_TOUCH        27   // broche Touch T7 (turbo)
#define PIN_LED           2   // LED bonus

// ============================================================
//  3) SEUILS ET TEMPORISATIONS
// ============================================================

// --- Saut (HC-SR04) ---
#define JUMP_DISTANCE_CM        15      // sous cette distance = saut
#define JUMP_CHECK_INTERVAL_MS  50      // on mesure toutes les 50 ms
#define JUMP_COOLDOWN_MS        1000    // pause après un saut publié

// --- Volant (potentiomètre) ---
#define STEER_INTERVAL_MS       50
#define STEER_LEFT_MAX_PCT      30      // 0..30  % -> left
#define STEER_RIGHT_MIN_PCT     70      // 70..100 % -> right

// --- Température (DHT11) ---
#define TEMP_INTERVAL_MS        2000    // le DHT11 ne peut pas aller plus vite

// --- Turbo (Touch) ---
#define TOUCH_THRESHOLD         35      // TODO étape 4d : à calibrer !
#define TOUCH_INTERVAL_MS       50
#define TURBO_DURATION_MS       3000    // turbo actif 3 s
#define TURBO_COOLDOWN_MS       5000    // puis pause 5 s

// --- Réseau ---
#define WIFI_TIMEOUT_MS         10000
#define RECONNECT_INTERVAL_MS   5000    // pas plus d'une tentative / 5 s

// --- Mode calibration du turbo : mets 1 pour afficher touchRead() ---
#define CALIBRATION_MODE        0

// --- Bonus (étape 3 des missions bonus) : mets 1 pour activer ---
#define BONUS_SUBSCRIBER        0

// ============================================================
//  4) OBJETS ET VARIABLES GLOBALES (déjà prêts)
// ============================================================
DHT dht(PIN_DHT, DHT11);
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

char clientId[32];            // ex. "esp32-G1"
char topicDirection[64];      // iot-racing/G1/direction
char topicJump[64];           // iot-racing/G1/jump
char topicTemperature[64];    // iot-racing/G1/temperature
char topicTurbo[64];          // iot-racing/G1/turbo
char topicGame[64];           // iot-racing/game (bonus subscriber)
char topicTest[64];           // iot-racing/G1/test (étape 3)

// --- Mémoires de chaque fonction (pour le non bloquant) ---
unsigned long lastJumpCheck   = 0;
unsigned long lastJumpPublish = 0;
bool          objectWasClose  = false;

unsigned long lastSteerRead   = 0;
String        lastDirection   = "";      // vide = rien publié encore

unsigned long lastTempRead    = 0;

enum TurboState { TURBO_READY, TURBO_ACTIVE, TURBO_COOLDOWN };
TurboState    turboState      = TURBO_READY;
unsigned long turboStateSince = 0;
unsigned long lastTouchRead   = 0;

unsigned long lastTestPublish = 0;

// ============================================================
//  5) FONCTIONS UTILITAIRES (déjà écrites, ne pas modifier)
// ============================================================

// Construit les topics à partir du préfixe et de TEAM_ID.
void buildTopics() {
  snprintf(clientId,         sizeof(clientId),         "esp32-%s", TEAM_ID);
  snprintf(topicDirection,   sizeof(topicDirection),   "%s%s/direction",   TOPIC_PREFIX, TEAM_ID);
  snprintf(topicJump,        sizeof(topicJump),        "%s%s/jump",        TOPIC_PREFIX, TEAM_ID);
  snprintf(topicTemperature, sizeof(topicTemperature), "%s%s/temperature", TOPIC_PREFIX, TEAM_ID);
  snprintf(topicTurbo,       sizeof(topicTurbo),       "%s%s/turbo",       TOPIC_PREFIX, TEAM_ID);
  snprintf(topicTest,        sizeof(topicTest),        "%s%s/test",        TOPIC_PREFIX, TEAM_ID);
  snprintf(topicGame,        sizeof(topicGame),        "%sgame",           TOPIC_PREFIX);
}

// Publie un message texte. Renvoie true si le message est parti.
// Renvoie false (sans planter) si le MQTT n'est pas connecté.
bool publishMsg(const char* topic, const char* payload) {
  if (!mqtt.connected()) return false;
  bool ok = mqtt.publish(topic, payload);
  Serial.printf("[PUB] %s -> %s (%s)\n", topic, payload, ok ? "ok" : "ECHEC");
  return ok;
}

// Mesure la distance en cm avec le HC-SR04.
// Renvoie -1 si rien n'est détecté (trop loin / pas d'écho).
float measureDistanceCm() {
  digitalWrite(PIN_TRIG, LOW);
  delayMicroseconds(2);
  digitalWrite(PIN_TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(PIN_TRIG, LOW);
  unsigned long duration = pulseIn(PIN_ECHO, HIGH, 25000UL);  // max 25 ms
  if (duration == 0) return -1;
  return duration * 0.0343 / 2.0;     // vitesse du son : 0,0343 cm/µs, aller-retour
}

// Reconnecte Wi-Fi puis MQTT si besoin, au plus toutes les 5 s.
// Appelée à chaque tour de loop(). Elle appelle TES fonctions.
void ensureConnections() {
  static unsigned long lastAttempt = 0;
  bool wifiOk = (WiFi.status() == WL_CONNECTED);
  if (wifiOk && mqtt.connected()) return;                       // tout va bien
  if (millis() - lastAttempt < RECONNECT_INTERVAL_MS) return;   // trop tôt
  lastAttempt = millis();

  if (!wifiOk) connectWiFi();
  if (WiFi.status() == WL_CONNECTED && !mqtt.connected()) connectMQTT();
}

// ============================================================
//  6) TES FONCTIONS : À COMPLÉTER
// ============================================================

/*
 * ÉTAPE 1 - connectWiFi()
 * Connecte l'ESP32 au Wi-Fi. Renvoie true si connecté, false sinon.
 */
bool connectWiFi() {
  Serial.printf("[WIFI] Connexion à %s...\n", WIFI_SSID);

  // TODO 1 : mets l'ESP32 en mode "station" (WiFi.mode(WIFI_STA))
  // TODO 2 : lance la connexion (WiFi.begin avec le SSID et le mot de passe)
  // TODO 3 : attends que WiFi.status() == WL_CONNECTED, mais pas plus de
  //          WIFI_TIMEOUT_MS. Utilise millis(), pas delay() !
  //          Astuce : while (condition && millis() - debut < WIFI_TIMEOUT_MS) { }
  // TODO 4 : si connecté, affiche l'adresse IP (WiFi.localIP()) et renvoie true
  //          sinon affiche un message d'erreur et renvoie false

  return false;   // à remplacer
}

/*
 * ÉTAPE 2 - connectMQTT()
 * Se connecte au broker avec le client ID unique de l'équipe.
 * Renvoie true si connecté.
 * (mqtt.setServer(...) est déjà fait dans setup())
 */
bool connectMQTT() {
  Serial.printf("[MQTT] Connexion à %s:%d (client %s)...\n", MQTT_BROKER, MQTT_PORT, clientId);

  // TODO 1 : appelle mqtt.connect(...) avec clientId
  // TODO 2 : si ça marche, affiche "Connecté au broker" et renvoie true
  //          sinon affiche le code d'erreur mqtt.state() et renvoie false
  //          (codes : https://pubsubclient.knolleary.net/api#state)

#if BONUS_SUBSCRIBER
  // BONUS : abonne-toi au topic du jeu : mqtt.subscribe(topicGame);
#endif

  return false;   // à remplacer
}

/*
 * ÉTAPE 4c - checkJump()
 * Si la distance passe sous JUMP_DISTANCE_CM, publie "1" sur topicJump.
 * UN SEUL message par saut, puis JUMP_COOLDOWN_MS de pause.
 */
void checkJump() {
  if (millis() - lastJumpCheck < JUMP_CHECK_INTERVAL_MS) return;
  lastJumpCheck = millis();

  float distance = measureDistanceCm();           // -1 si rien détecté

  // TODO 1 : objectIsClose = vrai si distance > 0 ET distance < JUMP_DISTANCE_CM
  // TODO 2 : détecte le PASSAGE sous le seuil : objectIsClose est vrai
  //          maintenant ET objectWasClose était faux au tour précédent
  // TODO 3 : si passage détecté ET JUMP_COOLDOWN_MS écoulé depuis lastJumpPublish :
  //          publishMsg(topicJump, "1") et mémorise l'heure dans lastJumpPublish
  // TODO 4 : mémorise objectIsClose dans objectWasClose pour le prochain tour
}

/*
 * ÉTAPE 4a - readSteering()
 * Lit le potentiomètre, convertit en left / center / right
 * et publie sur topicDirection UNIQUEMENT quand la direction change.
 */
void readSteering() {
  if (millis() - lastSteerRead < STEER_INTERVAL_MS) return;
  lastSteerRead = millis();

  // TODO 1 : lis la valeur brute avec analogRead(PIN_POT)  (0..4095)
  // TODO 2 : convertis en pourcentage 0..100 (indice : map())
  // TODO 3 : détermine la direction :
  //            pourcentage < STEER_LEFT_MAX_PCT  -> "left"
  //            pourcentage < STEER_RIGHT_MIN_PCT -> "center"
  //            sinon                             -> "right"
  // TODO 4 : si la direction est différente de lastDirection :
  //            publie-la sur topicDirection,
  //            et mets à jour lastDirection SEULEMENT si publishMsg() a renvoyé true
  //            (sinon le changement serait perdu pendant une coupure réseau)
}

/*
 * ÉTAPE 4b - readTemperature()
 * Lit le DHT11 toutes les TEMP_INTERVAL_MS et publie sur topicTemperature.
 * Ce message sert aussi de "signal de vie" pour le dashboard.
 */
void readTemperature() {
  if (millis() - lastTempRead < TEMP_INTERVAL_MS) return;
  lastTempRead = millis();

  // TODO 1 : lis la température avec dht.readTemperature()
  // TODO 2 : si le résultat est invalide (isnan(...)), affiche une erreur série et sors
  // TODO 3 : convertis en texte avec 1 décimale (ex. "28.5")
  //          indice : String(valeur, 1).c_str()
  // TODO 4 : publishMsg(topicTemperature, texte)
}

/*
 * ÉTAPE 4d - checkTurbo()
 * Machine à 3 états : READY -> ACTIVE (3 s) -> COOLDOWN (5 s) -> READY
 * Sur l'ESP32, touchRead() DIMINUE quand on touche.
 */
void checkTurbo() {
  if (millis() - lastTouchRead < TOUCH_INTERVAL_MS) return;
  lastTouchRead = millis();

  int value = touchRead(PIN_TOUCH);
  bool touched = (value < TOUCH_THRESHOLD);

  switch (turboState) {
    case TURBO_READY:
      // TODO 1 : si touched : publishMsg(topicTurbo, "1"),
      //          passe à TURBO_ACTIVE et mémorise l'heure dans turboStateSince
      break;

    case TURBO_ACTIVE:
      // TODO 2 : si TURBO_DURATION_MS s'est écoulé depuis turboStateSince :
      //          passe à TURBO_COOLDOWN et mémorise l'heure
      break;

    case TURBO_COOLDOWN:
      // TODO 3 : si TURBO_COOLDOWN_MS s'est écoulé depuis turboStateSince :
      //          repasse à TURBO_READY
      break;
  }
}

#if BONUS_SUBSCRIBER
/*
 * BONUS - callback appelé à chaque message reçu (voir setup()).
 * TODO : si topic == topicGame et message == "start", allume la LED ;
 *        si "stop", éteins-la.
 */
void onMqttMessage(char* topic, byte* payload, unsigned int length) {
  String message;
  for (unsigned int i = 0; i < length; i++) message += (char)payload[i];
  Serial.printf("[SUB] %s -> %s\n", topic, message.c_str());
  // TODO
}
#endif

// ============================================================
//  7) setup() ET loop() (déjà prêts)
// ============================================================

void setup() {
  Serial.begin(115200);
  delay(500);                       // delay autorisé ici : on est dans setup()

  pinMode(PIN_TRIG, OUTPUT);
  pinMode(PIN_ECHO, INPUT);
  pinMode(PIN_LED, OUTPUT);
  analogSetPinAttenuation(PIN_POT, ADC_11db);   // lecture sur toute la plage 0..3,3 V
  dht.begin();

  buildTopics();
  mqtt.setServer(MQTT_BROKER, MQTT_PORT);
#if BONUS_SUBSCRIBER
  mqtt.setCallback(onMqttMessage);
#endif

  Serial.println("\n=== MQTT IoT Racing ===");
  Serial.printf("Équipe : %s | Client ID : %s\n", TEAM_ID, clientId);

  connectWiFi();
  if (WiFi.status() == WL_CONNECTED) connectMQTT();
}

void loop() {
  // --- Mode calibration : on n'affiche que la valeur du capteur Touch ---
#if CALIBRATION_MODE
  static unsigned long lastPrint = 0;
  if (millis() - lastPrint > 200) {
    lastPrint = millis();
    Serial.printf("touchRead(%d) = %d\n", PIN_TOUCH, touchRead(PIN_TOUCH));
  }
  return;
#endif

  // 1) Maintenir les connexions (ne jamais bloquer)
  ensureConnections();
  mqtt.loop();                      // indispensable : garde le MQTT en vie

  // 2) ÉTAPE 3 : premier publish, toutes les 5 s, sans delay()
  //    TODO : décommente, puis comprends chaque ligne
  // if (millis() - lastTestPublish >= 5000) {
  //   lastTestPublish = millis();
  //   publishMsg(topicTest, "hello");
  // }

  // 3) Capteurs : décommente UNE ligne à la fois, au fil des étapes
  // checkJump();          // étape 4c
  // readSteering();       // étape 4a
  // readTemperature();    // étape 4b
  // checkTurbo();         // étape 4d
}
