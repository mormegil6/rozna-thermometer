// No need for alarms
#define REQUIRESALARMS false

#include <OneWire.h>
#include <DallasTemperature.h>
#include <EtherCard.h>

// Data wire is plugged into port 18
#define ONE_WIRE_BUS 10

// Setup a oneWire instance to communicate with any OneWire devices (not just Maxim/Dallas temperature ICs)
OneWire oneWire(ONE_WIRE_BUS);

// Pass our oneWire reference to Dallas Temperature. 
DallasTemperature sensors(&oneWire);

// ethernet interface mac address - must be unique on your network
static byte mymac[] = { 
  0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED };
// ethernet interface static ip address
static byte myip[] = { 
  192,168,1,200 };
// ethernet interface static gateway ip address
static byte gwip[] = { 
  192,168,1,1 };

// tcp/ip send and receive buffer
byte Ethernet::buffer[1000];   
// used as cursor while filling the buffer
static BufferFiller bfill;  

static int freeRam () {
  extern int __heap_start, *__brkval; 
  int v; 
  return (int) &v - (__brkval == 0 ? (int) &__heap_start : (int) __brkval); 
}

// Infinite loop that blink led on pin
void errorLoop(int pin, int dly) {
  pinMode(pin, OUTPUT);
  while (true) {
    digitalWrite(pin, HIGH);
    delay(dly);
    digitalWrite(pin, LOW);
    delay(dly);
  }
}

void setup(){
  pinMode(8, OUTPUT);
  pinMode(9, OUTPUT);
  pinMode(10, OUTPUT);
  digitalWrite(10, HIGH);

  pinMode(6, INPUT_PULLUP);
  delay(10);
  boolean useDhcp = (digitalRead(6) == HIGH);

  // Start the onw wire bus
  sensors.begin();
  sensors.setResolution(12);
  //digitalWrite(9, sensors.isParasitePowerMode() ? HIGH : LOW);

  // Start the ethernet with CS on pin 12 for JY-MCU
  if (ether.begin(sizeof Ethernet::buffer, mymac, 12) == 0) 
    errorLoop(8, 100);

  if (useDhcp) {
    while (!ether.dhcpSetup());
  } 
  else {
    ether.staticSetup(myip, gwip);
  }

  digitalWrite(10, LOW);
}

void writeHeaders(BufferFiller& buf) {
  buf.print(F("HTTP/1.0 200 OK\r\nPragma: no-cache\r\n"));
}

void homePage(BufferFiller& buf) {
  writeHeaders(buf);
  buf.println(F("Content-Type: text/html\r\n"));
  buf.print(F(
    "<!DOCTYPE html><html><head>"
    "<meta name='viewport' content='width=device-width, initial-scale=1' />"
    "<title>TempServer</title>"
    "<link rel='stylesheet' href='//code.jquery.com/mobile/1.2.0/jquery.mobile-1.2.0.min.css' />"
    "<link rel='stylesheet' href='main.css' />"
    "<script src='//code.jquery.com/jquery-1.8.2.min.js'></script>"
    "<script src='//code.jquery.com/mobile/1.2.0/jquery.mobile-1.2.0.min.js'></script>"
    "<script src='//stevenlevithan.com/assets/misc/date.format.js'></script>"
    "<script src='main.js'></script>"
    "</head><body><div id='main' data-role='page'>"
    "<div data-role='header' data-position='fixed'><h3>Is it hot?</h3></div>"
    "<div data-role='content'>"
    "<ul id='list' data-role='listview' data-inset='true'></ul>"
    "<p id='info'></p></div></div></body></html>"));
}

void mainCss(BufferFiller& buf) {
  writeHeaders(buf);
  buf.println(F("Content-Type: text/css\r\n"));
  buf.print(F(
    ".ui-li-aside{font-weight:bold;font-size:xx-large;}"
    ".ui-li-aside > sup{font-size:large;}"
    "#info{margin-top:10px;text-align:center;font-size:small;}"));
}

void mainJs(BufferFiller& buf) {
  writeHeaders(buf);
  buf.println(F("Content-Type: application/javascript\r\n"));
  buf.print(F(
    "$(document).ready(function(){reload()});function reload(){$.getJSON('list.json',function(c)"
    "{var d=[];$.each(c.list,function(a,b){d.push('<li id=\"'+b.id+'\"><a><h3>'+b.name+'</h3>"
    "<p>'+b.id+'</p><p class=\"ui-li-aside\">'+b.val.toFixed(1)+'<sup>&deg;C</sup></p></a></li>')});"
    "$('#list').html(d.join('')).trigger('create').listview('refresh');var e=new Date(c.uptime);"
    "$('#info').html('Uptime: '+e.format('isoTime')+' ('+c.free+' bytes free)')});setTimeout(reload,10000)}"));
}

void listJson(BufferFiller& buf) {
  writeHeaders(buf);
  buf.println(F("Content-Type: application/json\r\n"));
  buf.print(F("{\"list\":["));
    
  int index = 1;
  
  DeviceAddress addr;
  sensors.requestTemperatures();
  oneWire.reset_search();
  while (oneWire.search(addr)) {
    if (index != 1) buf.write(',');
    float tempC = sensors.getTempC(addr);
    buf.emit_p(PSTR("{\"id\":\"$H$H$H$H$H$H$H$H\",\"name\":\"Sensor $D\",\"val\":$T}")
      , addr[0], addr[1], addr[2], addr[3], addr[4], addr[5], addr[6], addr[7], index, tempC);
    index++;
  }

  buf.emit_p(PSTR("],\"uptime\":$L,\"free\":$D}"), millis(), freeRam());
}

boolean checkUrl(const __FlashStringHelper *val, const char* data) {
  const char PROGMEM *p = (const char PROGMEM *)val;
  while (1) {
    char c = pgm_read_byte(p++);
    if (c == 0) break;
    if (*data != c) return false;
    data++;
  }
  return true;
}

void loop(){
  digitalWrite(8, millis()/1000%2 ? HIGH : LOW);

  word len = ether.packetReceive();
  word pos = ether.packetLoop(len);
  // check if valid tcp data is received
  if (pos) {
    bfill = ether.tcpOffset();
    char* data = (char *) Ethernet::buffer + pos;

    // receive buf hasn't been clobbered by reply yet
    if (checkUrl(F("GET / "), data))
      homePage(bfill);
    else if (checkUrl(F("GET /main.css "), data))
      mainCss(bfill);
    else if (checkUrl(F("GET /main.js "), data))
      mainJs(bfill);
    else if (checkUrl(F("GET /list.json "), data))
     listJson(bfill);     
    else
      bfill.print(F(
        "HTTP/1.0 404 Not Found\r\n"
        "Content-Type: text/html\r\n"
        "\r\n"
        "<h1>404 Not Found</h1>"));  
    ether.httpServerReply(bfill.position()); // send web page data
  }
}





