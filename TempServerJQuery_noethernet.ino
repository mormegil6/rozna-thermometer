#define REQUIRESALARMS false // No need for alarms

#include <OneWire.h>
#include <DallasTemperature.h>
#include <EtherCard.h>
#include <LiquidCrystal.h>
#include <Time.h>

#define ONE_WIRE_BUS 10 // Data wire is plugged into port 10 (18)
OneWire oneWire(ONE_WIRE_BUS); // Setup a oneWire instance to communicate with any OneWire devices (not just Maxim/Dallas temperature ICs)
DallasTemperature sensors(&oneWire); // Pass our oneWire reference to Dallas Temperature. 

LiquidCrystal lcd(7, 6, 5, 4, 3, 2);
float celsius, fahrenheit; //celsius1, celsius2
DeviceAddress addr1;
DeviceAddress addr2;
unsigned long TimeMark=0;
unsigned long int TimeInterval=1499;
#define TIME_HEADER  "T"   // Header tag for serial time sync message
#define TIME_REQUEST  7    // ASCII bell character requests a time sync message 

byte mymac[] = {  0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED };
static byte myip[] = { 192,168,1,200 };
static byte gwip[] = { 192,168,1,1 };

byte Ethernet::buffer[1000]; // tcp/ip send and receive buffer  
static BufferFiller bfill; // used as cursor while filling the buffer

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
  
  ether.staticSetup(myip, gwip);
  sensors.begin(); // Start the onw wire bus
  lcd.begin(16, 2);
  while (!Serial) ; // Needed for Leonardo only
  pinMode(12, OUTPUT);
  setSyncProvider(requestSync);  //set function to call when sync required
  Serial.println("Waiting for sync message"); // sudo echo "T$(($(date +%s)+60*60))" >/dev/ttyACM0
  sensors.setResolution(12);
  //digitalWrite(9, sensors.isParasitePowerMode() ? HIGH : LOW);
  
//  if (ether.begin(sizeof Ethernet::buffer, mymac) == 0) {
//    Serial.println(F("Failed to access Ethernet controller"));
//    errorLoop(11, 100);
//  }
  
}

void writeHeaders(BufferFiller& buf) {
  buf.print(F("HTTP/1.0 200 OK\r\nPragma: no-cache\r\n"));
}

void homePage(BufferFiller& buf) {
  writeHeaders(buf);
  buf.println(F("Content-Type: text/html\r\n"));
  buf.print(F(
    "<!DOCTYPE html><html><head>"
    "<meta charset=\"utf-8\">"
    "<meta name='viewport' content='width=device-width, initial-scale=1' />"
    "<link rel=\"shortcut icon\" href=\"http://www.iconsdb.com/icons/download/purple/temperature-16.ico\">"
    "<title>Arduino TempServer</title>"
    "<link rel='stylesheet' href='//code.jquery.com/mobile/1.2.0/jquery.mobile-1.2.0.min.css' />"
    "<link rel='stylesheet' href='main.css' />"
    "<script src='//code.jquery.com/jquery-1.8.2.min.js'></script>"
    "<script src='//code.jquery.com/mobile/1.2.0/jquery.mobile-1.2.0.min.js'></script>"
    "<script src='//stevenlevithan.com/assets/misc/date.format.js'></script>"
    "<script src='main.js'></script>"
    "</head><body><div id='main' data-role='page'>"
    "<div data-role='header' data-position='fixed'><h3>Rožna Arduino<br>Thermometer</h3></div>"
    "<div data-role='content'>"
    "<ul id='list' data-role='listview' data-inset='true'></ul>"
    //"<IMG class=\"image\" src=\"http://s3.postimg.org/6dv8q3nbz/ardu.png\">" //<img src=\"http://s3.postimg.org/lnv3xaiu7/openwrt_logo_med.png\"><img src=\"http://s3.postimg.org/gmj4i6lzz/lmlogo.png\"><img src=\"http://s3.postimg.org/o6gsxz4kf/opensource.png\">
    "<p id='info'></p></div></div></body></html>"));
}

void mainCss(BufferFiller& buf) {
  writeHeaders(buf);
  buf.println(F("Content-Type: text/css\r\n"));
  buf.print(F(
    ".ui-li-aside{font-weight:bold;font-size:xx-large;}"
    ".ui-li-aside > sup{font-size:large;}"
    "IMG.image{display:block;margin-left:auto;margin-right:auto;}"
    //"#info{margin-top:10px;text-align:center;font-size:small;position:relative;bottom:0px;}"));
    "#info{bottom:0;position:fixed;text-align:center;width:100%;margin-top:10px;font-size:small;}"));
}

void mainJs(BufferFiller& buf) {
  writeHeaders(buf);
  buf.println(F("Content-Type: application/javascript\r\n"));
  buf.print(F(
    "$(document).ready(function(){reload()});function reload(){$.getJSON('list.json',function(c)"
    "{var d=[];$.each(c.list,function(a,b){d.push('<li id=\"'+b.id+'\"><a><h3>'+b.name+'</h3>"
    "<p>'+b.id+'</p><p class=\"ui-li-aside\">'+b.ifnegative+''+b.val.toFixed(1)/100+'<sup>&deg;C</sup></p></a></li>')});"
    "$('#list').html(d.join('')).trigger('create').listview('refresh');var e=new Date(c.uptime);"
    "$('#info').html('<IMG class=\"image\" src=\"http://s3.postimg.org/6dv8q3nbz/ardu.png\"></br>Assembled by Bartłomiej Mróz</br>Uptime: '+e.format('isoTime')+' ('+c.free+' bytes free)')});setTimeout(reload,14974)}"));
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
    int tempCint = tempC*100;
    char* ifnegative = " ";
    if (tempCint < 0) {
      tempCint = tempCint * (-1);
      ifnegative = "-";
    }
    if (index == 1) {
      addr1[0] = addr[0]; addr1[1] = addr[1]; addr1[2] = addr[2]; addr1[3] = addr[3]; addr1[4] = addr[4]; addr1[5] = addr[5]; addr1[6] = addr[6]; addr1[7] = addr[7];
    } else if (index == 2) {
      addr2[0] = addr[0]; addr2[1] = addr[1]; addr2[2] = addr[2]; addr2[3] = addr[3]; addr2[4] = addr[4]; addr2[5] = addr[5]; addr2[6] = addr[6]; addr2[7] = addr[7];
    }
    buf.emit_p(PSTR("{\"id\":\"$D$D$D$D$D$D$D$D\",\"name\":\"Sensor $D\",\"val\":$D,\"ifnegative\":\"$S\"}")
      , addr[0], addr[1], addr[2], addr[3], addr[4], addr[5], addr[6], addr[7], index, tempCint, ifnegative);
    index++;
  }

  buf.emit_p(PSTR("],\"uptime\":$L,\"free\":$D}"), millis() - 3600000, freeRam());
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
  //digitalWrite(8, millis()/1000%2 ? HIGH : LOW);
  
//  word len = ether.packetReceive();
//  word pos = ether.packetLoop(len);
  
//  // check if valid tcp data is received
//  if (pos) {
//    bfill = ether.tcpOffset();
//    char* data = (char *) Ethernet::buffer + pos;
//
//    // receive buf hasn't been clobbered by reply yet
//    if (checkUrl(F("GET / "), data)) {
//      homePage(bfill);
//    }
//    else if (checkUrl(F("GET /main.css "), data)) {
//      mainCss(bfill);
//    }
//    else if (checkUrl(F("GET /main.js "), data)) {
//      mainJs(bfill);
//    }
//    else if (checkUrl(F("GET /list.json "), data)) {
//     listJson(bfill);
//    }
//    else
//      bfill.print(F(
//        "HTTP/1.0 404 Not Found\r\n"
//        "Content-Type: text/html\r\n"
//        "\r\n"
//        "<h1>404 Not Found</h1>"));
//    ether.httpServerReply(bfill.position()); // send web page data
//  }
  
  if (IsTime(&TimeMark,TimeInterval)) {
    
    sensors.requestTemperatures();
    //celsius = sensors.getTempC(addr1);
    celsius = sensors.getTempCByIndex(0);
    fahrenheit = DallasTemperature::toFahrenheit(celsius);
    //celsius2 = sensors.getTempC(addr2);
    lcd.clear();
    lcd.setCursor(0,0);
    lcdprintTemp(celsius);
    lcd.print((char)223);
    lcd.print("C ");
    lcdprintDigits(hour());
    lcd.print(":");
    lcdprintDigits(minute());
    lcd.print(":");
    lcdprintDigits(second());
    
    lcd.setCursor(0,1);
    lcdprintTemp(fahrenheit);
    lcd.print((char)223);
    lcd.print("F ");
    lcdprintDigits(day());
    lcd.print(".");
    lcdprintDigits(month());
    lcd.print(".");
    lcd.print(year() - 2000);
    
    if (Serial.available()) {
      processSyncMessage();
    }
    if (timeStatus() == timeSet) {
      digitalWrite(12, HIGH); // LED on if synced
    } else {
      digitalWrite(12, LOW);  // LED off if needs refresh
    }
  }
  
}

//// >>>>> TimeDisplay functions <<<<< //
//void digitalClockDisplay(){
//  // digital clock display of the time
//  Serial.print(hour());
//  printDigits(minute());
//  printDigits(second());
//  Serial.print(" ");
//  Serial.print(day());
//  Serial.print(" ");
//  Serial.print(month());
//  Serial.print(" ");
//  Serial.print(year()); 
//  Serial.println(); 
//}
//
//void printDigits(int digits){
//  // utility function for digital clock display: prints preceding colon and leading 0
//  Serial.print(":");
//  if(digits < 10)
//    Serial.print('0');
//  Serial.print(digits);
//}

void lcdprintDigits(int digits){
  // utility function for digital clock display: prints preceding colon and leading 0
  //lcd.print(":");
  if(digits < 10)
    lcd.print('0');
  lcd.print(digits);
}

void lcdprintTemp(float digits){
  if(digits < 10 && digits >= 0)
    lcd.print('0');
  lcd.print(digits);
}

void processSyncMessage() {
  unsigned long pctime;
  const unsigned long DEFAULT_TIME = 1357041600; // Jan 1 2013

  if(Serial.find(TIME_HEADER)) {
     pctime = Serial.parseInt();
     if( pctime >= DEFAULT_TIME) { // check the integer is a valid time (greater than Jan 1 2013)
       setTime(pctime); // Sync Arduino clock to the time received on the serial port
     }
  }
}

time_t requestSync()
{
  Serial.write(TIME_REQUEST);  
  return 0; // the time will be sent later in response to serial mesg
}

// >>>>> IsTime functions <<<<< //
#define TIMECTL_MAXTICKS  4294967295L
#define TIMECTL_INIT      0
int IsTime(unsigned long *timeMark, unsigned long timeInterval){
  unsigned long timeCurrent;
  unsigned long timeElapsed;
  int result=false;
  
  timeCurrent=millis();
  if(timeCurrent<*timeMark) { //Rollover detected
    timeElapsed=(TIMECTL_MAXTICKS-*timeMark)+timeCurrent; //elapsed=all the ticks to overflow + all the ticks since overflow
  }
  else {
    timeElapsed=timeCurrent-*timeMark;  
  }

  if(timeElapsed>=timeInterval) {
    *timeMark=timeCurrent;
    result=true;
  }
  return(result);  
}

// >>>>> ToString function <<<<< //
//char _int2str[7];
//char* int2str( register float i ) {
//  register unsigned char L = 1;
//  register char c;
//  register boolean m = false;
//  register char b;  // lower-byte of i
//  // negative
//  if ( i < 0 ) {
//    _int2str[ 0 ] = '-';
//    i = -i;
//  }
//  else L = 0;
//  // ten-thousands
//  if( i > 9999 ) {
//    c = i < 20000 ? 1
//      : i < 30000 ? 2
//      : 3;
//    _int2str[ L++ ] = c + 48;
//    i -= c * 10000;
//    m = true;
//  }
//  // thousands
//  if( i > 999 ) {
//    c = i < 5000
//      ? ( i < 3000
//          ? ( i < 2000 ? 1 : 2 )
//          :   i < 4000 ? 3 : 4
//        )
//      : i < 8000
//        ? ( i < 6000
//            ? 5
//            : i < 7000 ? 6 : 7
//          )
//        : i < 9000 ? 8 : 9;
//    _int2str[ L++ ] = c + 48;
//    i -= c * 1000;
//    m = true;
//  }
//  else if( m ) _int2str[ L++ ] = '0';
//  // hundreds
//  if( i > 99 ) {
//    c = i < 500
//      ? ( i < 300
//          ? ( i < 200 ? 1 : 2 )
//          :   i < 400 ? 3 : 4
//        )
//      : i < 800
//        ? ( i < 600
//            ? 5
//            : i < 700 ? 6 : 7
//          )
//        : i < 900 ? 8 : 9;
//    _int2str[ L++ ] = c + 48;
//    i -= c * 100;
//    m = true;
//  }
//  else if( m ) _int2str[ L++ ] = '0';
//  // decades (check on lower byte to optimize code)
//  b = char( i );
//  if( b > 9 ) {
//    c = b < 50
//      ? ( b < 30
//          ? ( b < 20 ? 1 : 2 )
//          :   b < 40 ? 3 : 4
//        )
//      : b < 80
//        ? ( i < 60
//            ? 5
//            : i < 70 ? 6 : 7
//          )
//        : i < 90 ? 8 : 9;
//    _int2str[ L++ ] = c + 48;
//    b -= c * 10;
//    m = true;
//  }
//  else if( m ) _int2str[ L++ ] = '0';
//  // last digit
//  _int2str[ L++ ] = b + 48;
//  // null terminator
//  _int2str[ L ] = 0;  
//  return _int2str;
//}

