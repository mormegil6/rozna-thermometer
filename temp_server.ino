#include <EtherCard.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <LiquidCrystal.h>
#include <Time.h>
//#include <Exosite.h>

byte mymac[] = {  0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED };
static byte myip[] = { 192,168,1,200 };
static byte gwip[] = { 192,168,1,1 };

#define ONE_WIRE_BUS 10              // Data wire is plugged into port 2 on the Arduino
OneWire oneWire(ONE_WIRE_BUS);       // Setup a oneWire instance to communicate with any OneWire devices (not just Maxim/Dallas temperature ICs)
DallasTemperature sensors(&oneWire); // Pass our oneWire reference to Dallas Temperature. 

LiquidCrystal lcd(7, 6, 5, 4, 3, 2);

float celsius, fahrenheit;
//int dzien, miesiac, rok, godzina, minuta, sekunda;
//char* godz;
//char* mint;
//char* sek;

#define TIME_HEADER  "T"   // Header tag for serial time sync message
#define TIME_REQUEST  7    // ASCII bell character requests a time sync message 

unsigned long TimeMark=0;             //a millisecond time stamp used by the IsTime() function. initialize to 0
unsigned long int TimeInterval=10000;  //How many milliseconds we want for the flash cycle. 1000mS is 1 second.
unsigned long TimeMark2=0;
unsigned long int TimeInterval2=30000;

static void gotPinged (byte* ptr) {
  ether.printIp(">>> ping from: ", ptr);
  Serial.print(">>> ");
  digitalClockDisplay();
}

byte Ethernet::buffer[1000];
static BufferFiller bfill;

//String cikData = "f359bb527b8c9481d28389346db97f78128a5a15";
//class EthernetClient client;
//Exosite exosite(cikData, &client);

void setup () {
  if (ether.begin(sizeof Ethernet::buffer, mymac) == 0) {
    Serial.println(F("Failed to access Ethernet controller"));
  }
  ether.staticSetup(myip, gwip);
  ether.registerPingCallback(gotPinged);
  sensors.begin(); // Start up the library
  lcd.begin(16, 2);
  while (!Serial) ; // Needed for Leonardo only
  pinMode(13, OUTPUT);
  setSyncProvider(requestSync);  //set function to call when sync required
  Serial.println("Waiting for sync message"); // sudo echo "T$(($(date +%s)+60*60))" >/dev/ttyACM0
}

word homePage() {
  bfill = ether.tcpOffset();
  bfill.emit_p(PSTR(
    "HTTP/1.0 200 OK\r\n"
    "Content-Type: text/html\r\n"
    "Pragma: no-cache\r\n"
    "\r\n"
    "<meta http-equiv='' content='1'/>"
    "<title>My Arduino Website</title>" 
    "</br><h1>Hello !</h1></br>"
    "<p><em>"
      "blablabla<br/>"
      "some text<br/>"
      "people should dance<br/>"
      "and sing<br/>"
      "and some of them should RIDE<br/>"
      "<br/>"
      "<font size = 2>this website were supposed to display current time, date and temperature but I don't know how to send themit correctly :/ </font>"
      "<br/><br/>"
      //"<img src='https://lh3.googleusercontent.com/-PreA4mOTi6s/VMdi1KaAv-I/AAAAAAAAJMU/eYX6mOD19kY/w459-h194-no/ArduinoCommunityLogo3.jpg'>"
      //"<img src='https://lh4.googleusercontent.com/-cliaY_5MfWY/VMdh_2iEc7I/AAAAAAAAJME/Gw-5MyFp7kI/w459-h194-no/ArduinoCommunityLogo2.gif'>"
      //"<img src='http://assets.hgbrasil.com/home/img/arduino-logo-hugodemiglio.png'>"
      //"<img src='http://arduino.cc/en/uploads/Trademark/ArduinoCommunityLogo.png'>"
      //"<img src='http://www.tog.ie/wp-content/uploads/2012/12/openwrt-logo.png'>"
      //"<img src='http://upload.wikimedia.org/wikipedia/commons/5/5c/Linux_Mint_Official_Logo.svg'>"
      //"<img src='http://opensource.org/files/osi_symbol.png'>"
    "</em></p>")
    //, celsius
    );
      //fahrenheit, godzina, minuta, sekunda, dzien, miesiac, rok, godz, mint, sek
  return bfill.position();
}

void loop () {
  word len = ether.packetReceive();
  word pos = ether.packetLoop(len);
  
  if(pos) {
    if (IsTime(&TimeMark,TimeInterval)) { // check if valid tcp data is received
      Serial.print("page loaded ");
      digitalClockDisplay();
      ether.httpServerReply(homePage()); // send web page data
    }
  }
  
  sensors.requestTemperatures(); // Send the command to get temperatures
  //Serial.println(sensors.getTempCByIndex(0));
  celsius = sensors.getTempCByIndex(0);
  
  fahrenheit = DallasTemperature::toFahrenheit(celsius);
  //fahrenheit = celsius * 1.8 + 32.0;
  lcd.clear();
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
  if (timeStatus()!= timeNotSet) {
    if (IsTime(&TimeMark2,TimeInterval2)) {
      Serial.print("# ");
      digitalClockDisplay();
      Serial.print("# ");
      Serial.print(celsius);
      Serial.print(" C");
      Serial.println();
    }
  }
  if (timeStatus() == timeSet) {
    digitalWrite(13, HIGH); // LED on if synced
  } else {
    digitalWrite(13, LOW);  // LED off if needs refresh
  }
  
//  sekunda = second();
//  minuta = minute();
//  godzina = hour();
//  dzien = day();
//  miesiac = month();
//  rok = year();
//  sek = int2str(sekunda);
//  mint = int2str(minuta);
//  godz = int2str(godzina);
  
}



// >>>>> TimeDisplay functions <<<<< //
void digitalClockDisplay(){
  // digital clock display of the time
  Serial.print(hour());
  printDigits(minute());
  printDigits(second());
  Serial.print(" ");
  Serial.print(day());
  Serial.print(" ");
  Serial.print(month());
  Serial.print(" ");
  Serial.print(year()); 
  Serial.println(); 
}

void printDigits(int digits){
  // utility function for digital clock display: prints preceding colon and leading 0
  Serial.print(":");
  if(digits < 10)
    Serial.print('0');
  Serial.print(digits);
}

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
//char* int2str( register int i ) {
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

