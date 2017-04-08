
////////////////////////////////////////////////////////////////////////////
// Rolling shutter management v0.6 (local, centralized and remote)
// written by Pete (june 2016)
//
// !! USE AT YOUR OWN RISK !!
//
// Please read the comments bellow and take extra care with
// the relay logic. If you don't you WILL fry your shutter !!
//
// 
// The purpose is obiously to open and close rolling shutters
// based on :
//     - the buttons pressed next to each one (up and down)
//     - the centralized buttons next to the entrance door (up and down)
//     - the request from the web server (up/down/stop)
//
// Each shutter is connected with 3 wires (up/down/phase), if the phase
// is connected to 'up' it will go up, if connected to 'down' it will go down.
// Up and Down should not be connected at the same time
//
// For each shutter, I have 2 push-like buttons ("go up" "go down").
// If up is pressed once, the shutter should go up
// If up is pressed a second time, the shutter should stop
// Same logic for down.
//
// Additionnally, you can set a timeout that will turn all relay off.
//
// The web server provide a simple API :
//        http://<IP>/shutter
//              Gives the status of all shutters
//
//       http://<IP>/shutter?<number>=<action>
//       http://<IP>/shutter?all=<action>
//               <number> Id of the rolling shutter
//               <action> can be up down of stop
//
//       http://<IP>/shutter?ignorebuttons=<bool>
//              Enable/Disable buttons next to each shutter
//
//       http://<IP>/shutter?ignorecentralizedbuttons=<bool>
//              Enable/Disable buttons at the entrance
//
//      Multiple actions can be requested at the same time :
//         example      http://<IP>/shutter?1=up&2=down&3=stop
//
////////////////////////////////////////////////////////////////////////////

#include <SPI.h>
#include <Dhcp.h>
#include <Dns.h>
#include <Ethernet.h>
#include <EthernetServer.h>
//#include <util.h>
#include <WString.h>
#include <EEPROM.h>

//////////////////////////
// This is how my relay works
// could be the opposite on some other hardware
// Be extra CAREFULL here, because the "stop" state
// should be when all relays are openned.
// If all relays are closed at the same time, this could fry your shutter !
//////////////////////////
  #define RELAY_CLOSED LOW
  #define RELAY_OPEN  HIGH
  
  #define BUTTON_PRESSED  HIGH
  #define BUTTON_RELEASED LOW
  
  // minimimun time in milliseconds between button pressed on one shutter
  // this prevent to switch relays too rapidly
  // (this can happen when input pins are not correctly
  // connected to the ground or vcc and occilate from the noise
  #define MIN_TIME_ACTION_MS 300
  
  // put back relays to stop state for the shutter, set to 0 to disable
  #define AUTO_STOP_TIMEOUT  60000 
  
  // manage dhcp disconnection
  #define DHCP_RENEW_TIMEOUT 43200000 //1000*3600*12
 
//////////////////////////
// how many shutters do you have ?
//////////////////////////
const int nbmaxitems = 11;
const int nbshutters = 4;

/////////////////////////
// Connected pins on your arduino
// REMARK: pins used by ethernet shield :
// UNO   4 10 11 12 13
// MEGA  4 10 50 51 52
// ledUp and ledDown are for relays and pushButtonUp/pushButtonDown for buttons 
// This is a [4 x nbmaxitems] matrix.
// nbshutters columns should be filled, plus the last column for the centralized buttons
int        ledUp[] =   {30, 32, 34, 36,  38,  40,  42,  44,  46,  48,  0 };
int        ledDown[] = {31, 33, 35, 37,  39,  41,  43,  45,  47,  49,  0 };
int pushButtonUp[] =   { 6,  8, 12, 14,  16,  18,  20,  22,  24,  26,  28 };
int pushButtonDown[] = { 7,  9, 13, 15,  17,  19,  21,  23,  25,  27,  29 };

// you can give a name here that will be displayed on the webpage
const char* names[] = { 
                        "Parents 1",
                        "Parents 2", 
                        "Bath", 
                        "Enfant 2,2",
                        "Enfant 2,1", 
                        "Enfant 1",
                        "Salon door", 
                        "Salon balcony",
                        "Salon living", 
                        "Dinig", 
                        "General" }; 


//////////////////////////
//  for button and relay
//////////////////////////
struct ITEM 
{ 
  int pin;
  boolean state;
};

//////////////////////////
// one SHUTTER has 2 buttons and 2 relays (up/down)
//////////////////////////
struct ROLLING_SHUTTER {
    ITEM buttons[2];
    ITEM relays[2];
    
    String strname;
    int last_action_button;
    
    unsigned long last_action_time_ms;
    
  };
  
//////////////////////////
// first (0) item is up
// and second (1) is down 
// this is used in the algorithm in order to find
// the "opposite" button or relay of 'up_down' by using '!up_down'
//////////////////////////
  #define ITEM_UP 0
  #define ITEM_DOWN 1

//////////////////////////
// actual data,
//////////////////////////
ROLLING_SHUTTER shutters[nbmaxitems];


  // EEPROM SETTINGS
 #define CONFIG_VERSION "sv1"
 #define CONFIG_START   32
 struct StoreStruct {
  // This is for mere detection if they are your settings
  char version[4];
  // global flag to ignore hardware buttons and only use webpage API
  int enable_buttons;
  int enable_centralized_buttons;
} g_storage = { 
  CONFIG_VERSION,
  1,
  1
};
 
//
String bufstr = String(100); //string for fetching data from address
unsigned long count = 0;
int g_debug = 0;
int g_enable_EEPROM = 0;

// Enter a MAC address and IP address for your controller below.
// The IP address will be dependent on your local network:
byte mac[] = {  0x90, 0xA2, 0xDA, 0x10, 0xAA, 0xD1 };

// Initialize the Ethernet server library
// with the IP address and port you want to use
// (port 80 is default for HTTP):
EthernetServer server(80);
unsigned long last_dhcp_renew = 0;



////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////
void trace_item(int index, int up_down, bool button)
{
      Serial.print("[");
      char tbs[16];
      sprintf(tbs, "%05d", count);
      Serial.print(tbs);
      Serial.print("]");
      Serial.print(button ? " button " : " relay  ");
      sprintf(tbs, "%02d", index);
      Serial.print(index);
      Serial.print(":");
      Serial.print(up_down == ITEM_UP ? "UP  " : "DOWN");
      Serial.print(" (");
      sprintf(tbs, "%02d", button ? shutters[index].buttons[up_down].pin : shutters[index].relays[up_down].pin);
      Serial.print(tbs);
      Serial.print(")");
      count++;
}

void trace_button(int index, int up_down)
{
      trace_item(index, up_down, true);
}

void trace_relay(int index, int up_down)
{
      trace_item(index, up_down, false);
}



////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////

void loadConfig() {
    if(!g_enable_EEPROM)
      return;
      
  // To make sure there are settings, and they are YOURS!
  // If nothing is found it will use the default settings.
  if (EEPROM.read(CONFIG_START + 0) == CONFIG_VERSION[0] &&
      EEPROM.read(CONFIG_START + 1) == CONFIG_VERSION[1] &&
      EEPROM.read(CONFIG_START + 2) == CONFIG_VERSION[2])
      {
        for (unsigned int t=0; t<sizeof(g_storage); t++)
          *((char*)&g_storage + t) = EEPROM.read(CONFIG_START + t);
          Serial.println("config loaded from EEPROM");
      }
      else
      {
         // settings aren't valid! will overwrite with default settings
        saveConfig();
      }
}

void saveConfig() 
{
   if(!g_enable_EEPROM)
      return;
      
  for (unsigned int t=0; t<sizeof(g_storage); t++)
  {
    EEPROM.write(CONFIG_START + t, *((char*)&g_storage + t));
    
      // and verifies the data
    if (EEPROM.read(CONFIG_START + t) != *((char*)&g_storage + t))
    {
      // error writing to EEPROM
      Serial.println("error while writing EEPROM");
      return;
    }
  }
    
    Serial.println("config saved to EEPROM");
}


////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////
void setup ()
{
  Serial.begin(9600);
  /* while (!Serial) {
    ; // wait for serial port to connect. Needed for Leonardo only
  }*/
  
  Serial.println("-- init --");
  
   loadConfig();
   
  char tbs[16];
  
  //init
  for(int i=0; i<nbmaxitems; i++)
  {
    shutters[i].buttons[ITEM_UP].pin = pushButtonUp[i];
    shutters[i].buttons[ITEM_UP].state = false;
    
    shutters[i].buttons[ITEM_DOWN].pin = pushButtonDown[i];
    shutters[i].buttons[ITEM_DOWN].state = false;
    
    shutters[i].relays[ITEM_UP].pin = ledUp[i];
    shutters[i].relays[ITEM_UP].state = false;
    
    shutters[i].relays[ITEM_DOWN].pin = ledDown[i];
    shutters[i].relays[ITEM_DOWN].state = false;
    
    shutters[i].last_action_button = -1;
    shutters[i].strname = names[i];
    shutters[i].last_action_time_ms = millis();
    
    if(i >= nbshutters && i < nbmaxitems-1)
    {
       shutters[i].relays[ITEM_UP].pin = 0;
       shutters[i].relays[ITEM_DOWN].pin = 0;
       shutters[i].buttons[ITEM_UP].pin = 0;
       shutters[i].buttons[ITEM_DOWN].pin = 0;
       continue;
    }
   
     Serial.print("Shutter  ");
     sprintf(tbs, "[%02d] ", i);
     Serial.print(tbs);
    
    
    if( shutters[i].buttons[ITEM_UP].pin > 0)
    {
      pinMode (shutters[i].buttons[ITEM_UP].pin, INPUT);
    
      Serial.print(" BUTTON_UP ");
      sprintf(tbs, "(%02d),", shutters[i].buttons[ITEM_UP].pin);
      Serial.print(tbs);
      
      delay(10);
      
      pinMode (shutters[i].buttons[ITEM_DOWN].pin, INPUT);
     
      Serial.print(" BUTTON_DOWN ");
      sprintf(tbs, "(%02d),", shutters[i].buttons[ITEM_DOWN].pin);
      Serial.print(tbs);
      delay(10);
    }
    
    if(i != nbshutters && shutters[i].relays[ITEM_UP].pin > 0)
    {
      pinMode (shutters[i].relays[ITEM_UP].pin, OUTPUT);
      digitalWrite(shutters[i].relays[ITEM_UP].pin, RELAY_OPEN);
      Serial.print(" RELAY_UP ");
      sprintf(tbs, "(%02d),", shutters[i].relays[ITEM_UP].pin);
      Serial.print(tbs);
      delay(10);
      
      pinMode (shutters[i].relays[ITEM_DOWN].pin, OUTPUT);
      digitalWrite(shutters[i].relays[ITEM_DOWN].pin, RELAY_OPEN);
   
      Serial.print(" RELAY_DOWN ");
      sprintf(tbs, "(%02d),", shutters[i].relays[ITEM_DOWN].pin);
      Serial.print(tbs);
      
      delay(10);
    }
    
    Serial.println("");
  }
  
   Serial.print("auto timeout : ");
   Serial.print(AUTO_STOP_TIMEOUT);
   Serial.println(" ms");
   
   Serial.print("enable buttons : ");
   Serial.println(g_storage.enable_buttons);
   
    Serial.print("enable centralized button : ");
   Serial.println(g_storage.enable_centralized_buttons);
  
   Serial.println("init server");
  Ethernet.begin(mac);
  server.begin();
  Serial.print("server is at ");
  Serial.println(Ethernet.localIP());
  last_dhcp_renew = millis();
  
  Serial.println("-- init done --");
}



////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////
void print_html_header(EthernetClient* client)
{
      client->println("<!DOCTYPE HTML>");
      client->println("<html>");
      client->println("<head>");
      
      client->println(" <STYLE type='text/css'>");
      client->println("* { margin:0 auto; padding:0; }");
      client->println("body { font-family:Tahoma,sans-serif,Verdana,Arial; color:black; font-size:14pt; }");
      client->println(".container {padding: 10px;    margin:10px; text-align: center; }");
      client->println("table {border:1px solid black; border-collapse:collapse; padding: 10px;}");
      client->println("a, a:active, a:hover, a:visited {color:black;   text-decoration:none; }");
      client->println("td {border: 1px solid black; padding: 7px; padding-top: 10px; min-width:60px; }");
      client->println(".button {border: 1px solid black; background-color: #888; padding: 2px; }");
      client->println(".button:hover, .button:active {color:white; background-color: #DDD; }");
      
      client->println(".options {text-align: center; border: 1px solid black; background-color: white; padding: 10px; margin-top:10px; max-width: 320px; }");
      client->println(".options > div { padding-top: 10px; }");
      
      client->println(" </STYLE>");

      client->println("<meta http-equiv='Content-type' content='text/html; charset=UTF-8' />");
      client->println("<meta http-equiv='refresh' content='30' />");
      client->println("<meta name='viewport' content='width=320, initial-scale=1.0, maximum-scale=1.0, user-scalable=0' />");
      client->println("<title>Shutters</title>");
      client->println("</head>");
      
      
      client->println("<body>");
      client->println("<script>");
      client->println("function SendAction(url) {  var xhttp = new XMLHttpRequest(); xhttp.onreadystatechange = function() {  if (this.readyState == 4 && this.status == 200) { window.location.reload(false);  } }; xhttp.open('GET', url, true); xhttp.send(); } ");
      client->println("</script>");
      
      client->println("<div class='container'>");
      
}

////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////
void print_html_footer(EthernetClient* client)
{
       client->println("</div>");  // container
       client->println("</body>");
       client->println("</html>");
}

////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////
void print_html_status(EthernetClient* client)
{
  
  
      client->println("<table><tr><td>shutter</td><td>action</td><td>state</td>");
      if(g_debug) client->println("<td>button up</td><td>button down</td><td>relay up</td><td>relay down</td>");
      client->println("</tr>");
      
      // output the value of each analog input pin
      for (int i = 0; i < nbmaxitems; i++) 
      {
        if( shutters[i].buttons[ITEM_UP].pin == 0)
          continue;
          
        boolean sread_up = shutters[i].relays[ITEM_UP].state;
        boolean sread_down = shutters[i].relays[ITEM_DOWN].state;
        client->println("<tr><td>");
        client->print(shutters[i].strname);

         client->print("</td><td>");
         /*client->print("<a href='?");
         client->print(i+1);
         client->print("=up'>UP</a>");
         client->print(" <a href='?");
            client->print(i+1);
            client->print("=stop'>STOP</a>");
            client->print(" <a href='?");
            client->print(i+1);
            client->print("=down'>DOWN</a>");*/
            
            client->print("<span class='button'><a href='#' onclick='SendAction(\"?");
            client->print(i+1);
            client->print("=up");
            client->print("\"); return false;'");
            client->print(">UP</a></span>");
            client->print(" ");
            client->print("<span class='button'><a href='#' onclick='SendAction(\"?");
            client->print(i+1);
            client->print("=stop");
            client->print("\"); return false;'");
            client->print(">STOP</a></span>");
            client->print(" ");
            client->print("<span class='button'><a href='#' onclick='SendAction(\"?");
            client->print(i+1);
            client->print("=down");
            client->print("\"); return false;'");
            client->print(">DOWN</a></span>");
 
        client->println("</td><td>");
        if(sread_up  && !sread_down)
        {
            client->print("UP ");
        }
        else if(!sread_up && sread_down)
        {
            client->print("DOWN");
        }
        else if(!sread_up  && !sread_down )
        {
            client->print("STOP");
        }
        else
        {
            client->print("????");
        }
        
        if(g_debug) 
        {
          client->print("</td><td>(");
          client->print(shutters[i].buttons[ITEM_UP].pin);
          client->print(") ");
          client->print(shutters[i].buttons[ITEM_UP].state);
      
          client->print("</td><td>(");
          client->print(shutters[i].buttons[ITEM_DOWN].pin);
          client->print(") ");
          client->print(shutters[i].buttons[ITEM_DOWN].state);
          
           client->print("</td><td>");
          // if(shutters[i].relays[ITEM_UP].pin > 0)
           {
             client->print("(");
             client->print(shutters[i].relays[ITEM_UP].pin);
             client->print(") ");
             client->print(shutters[i].relays[ITEM_UP].state);
           }
          
          client->print("</td><td>");
          //if(shutters[i].relays[ITEM_DOWN].pin > 0)
           {
             client->print("(");
             client->print(shutters[i].relays[ITEM_DOWN].pin);
             client->print(") ");
             client->print(shutters[i].relays[ITEM_DOWN].state);
          }
        }
        
        
       client->println("</td></tr>");
      }
      client->println("</table>");
      
      
      
 client->print("<div class='options'>");
  client->print("<div>Buttons are ");

 if(g_storage.enable_buttons)
  client->print("Enabled  : <span class='button'><a  href='#' onclick='SendAction(\"?enablebuttons=false\"); return false;' >Disable</a></span>");  
 else
  client->print("Disabled :  <span class='button'><a href='#' onclick='SendAction(\"?enablebuttons=true\"); return false;'  >Enable</a></span>");  
  client->println("</div>");
  
  client->print("<div>Centralized buttons ");
  if(g_storage.enable_centralized_buttons)
    client->print("Enabled  :   <span class='button'><a  href='#' onclick='SendAction(\"?enablecentralizedbuttons=false\"); return false;' >Disable</a></span>");  
  else
    client->print("Disabled :   <span class='button'><a href='#' onclick='SendAction(\"?enablecentralizedbuttons=true\"); return false;'  >Enable</a></span>");
  client->println("</div>");
  
  client->println("</div>"); // options
}


////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////
void
ActivateRelay(int index, int up_down, int value)
{
   boolean previous_state = shutters[index].relays[up_down].state;
   
    /* Serial.print("pin  ");
      Serial.print(shutters[index].relays[up_down].pin);
      Serial.print(" -> ");
      Serial.println(value);*/
      
   digitalWrite(shutters[index].relays[up_down].pin, value);
   shutters[index].relays[up_down].state = (value == RELAY_CLOSED);
   
   if(previous_state != shutters[index].relays[up_down].state)
   {   
     trace_relay(index, up_down);
     Serial.print(" set to ");
     Serial.print(shutters[index].relays[up_down].state ? "CLOSED" : "OPEN" );
     Serial.println("");
   }
}

////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////
void process_shutter_query(EthernetClient* client, String query)
{
    int sep = 0;
    unsigned long inow = millis();

    while(sep != -1)
    {
      int sep2 = query.indexOf("&", sep);
      String strparam = query.substring(sep, sep2);
      if(strparam.length() < 3)
      {
        if(sep2 == -1)
       {
          
         break;
       }
        sep= sep2+1;
        continue;
      }//if
        
      int equ = strparam.indexOf("=");
      if(equ != -1)
      {
           Serial.print("process: ");
           Serial.println(strparam);
          String strname = strparam.substring(0, equ);
          String strvalue = strparam.substring(equ+1);
          
          /*client->print("<div>PARAM ");
          client->print(strname);
          client->print(" VALUE ");
          client->print(strvalue);
          client->println("</div>");*/
          
          int iv = strname.toInt() - 1;
          
          if(strname == "enablebuttons")
          {
             if(strvalue == "true")
             {            
               g_storage.enable_buttons= 1;
             }
             else  if(strvalue == "false")
             {            
               g_storage.enable_buttons= 0;
             }
             
              saveConfig();
          }
          else if(strname == "enablecentralizedbuttons")
          {
            if(strvalue == "true")
             {            
               g_storage.enable_centralized_buttons= 1;
             }
             else  if(strvalue == "false")
             {            
               g_storage.enable_centralized_buttons= 0;
             }
             
              saveConfig();
          }
          else if(strname == "all" || iv == nbmaxitems-1)
          {
             if(strvalue == "stop")
            {
                for(int i = 0; i < nbshutters; i++)
                {
                  ActivateRelay(i, ITEM_DOWN, RELAY_OPEN);
                  ActivateRelay(i, ITEM_UP, RELAY_OPEN);
  
                  shutters[i].last_action_time_ms = inow;
                }
                shutters[nbmaxitems-1].relays[ITEM_UP].state = false;
                shutters[nbmaxitems-1].relays[ITEM_DOWN].state = false;
                shutters[nbmaxitems-1].last_action_time_ms = inow;
                  
             // if(g_debug) client->println("<div>all stop</div>");
            }
            else if(strvalue == "up")
            {
                for(int i = 0; i < nbshutters; i++)
                {
                  ActivateRelay(i, ITEM_DOWN, RELAY_OPEN);
                  ActivateRelay(i, ITEM_UP, RELAY_CLOSED);
                  
                  shutters[i].last_action_time_ms = inow;
                }
                
                shutters[nbmaxitems-1].relays[ITEM_UP].state = true;
                shutters[nbmaxitems-1].relays[ITEM_DOWN].state = false;
                shutters[nbmaxitems-1].last_action_time_ms = inow;
                
              //if(g_debug) client->println("<div>all up</div>");
            }
            else if(strvalue == "down")
            {
                for(int i = 0; i < nbshutters; i++)
                {
                  ActivateRelay(i, ITEM_UP, RELAY_OPEN);
                  ActivateRelay(i, ITEM_DOWN, RELAY_CLOSED);
                  
                  shutters[i].last_action_time_ms = inow;
                }
                
                shutters[nbmaxitems-1].relays[ITEM_UP].state = false;
                shutters[nbmaxitems-1].relays[ITEM_DOWN].state = true;
                shutters[nbmaxitems-1].last_action_time_ms = inow;
                
              //if(g_debug)  client->println("<div>all down</div>");
            }
          }//else
          
          
          if(iv < 0 || iv >= nbshutters)
          {
            if(sep2 == -1) 
            {
              
              break;
              }
            sep= sep2+1;
            continue;
          }
            
          if(strvalue == "stop")
          {             
              ActivateRelay(iv, ITEM_DOWN, RELAY_OPEN);
              ActivateRelay(iv, ITEM_UP, RELAY_OPEN);
              
              shutters[iv].last_action_time_ms = inow;
              //if(g_debug) client->println("<div>stop</div>");
          }
          else if(strvalue == "up")
          {
              ActivateRelay(iv, ITEM_DOWN, RELAY_OPEN);
              ActivateRelay(iv, ITEM_UP, RELAY_CLOSED);

              shutters[iv].last_action_time_ms = inow;
              //if(g_debug) client->println("<div>up</div>");
          }
          else if(strvalue == "down")
          {
              ActivateRelay(iv, ITEM_UP, RELAY_OPEN);
              ActivateRelay(iv, ITEM_DOWN, RELAY_CLOSED);
              
              shutters[iv].last_action_time_ms = inow;
              //if(g_debug) client->println("<div>down</div>");
          }
      }
      
      if(sep2 == -1) 
      {
        
        break;
      }
      sep= sep2+1;
    }
    
}

////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////
void process_action(EthernetClient* client, String action, String query)
{
    if(action == "/shutters")
    {
        process_shutter_query(client, query);
    }
    else
    {
        Serial.print("unknown action : ");
        Serial.println(action);
    }
}


////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////
unsigned long millis_diff(unsigned long inow, unsigned long iref)
{
  if(inow >= iref)
  {
     return inow - iref;
  }
  
  return ((unsigned long)(-1) - iref) + inow;
}

////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////
void manage_client()
{
  unsigned long now = millis();
  unsigned long diff = millis_diff(now, last_dhcp_renew);
  
  if( diff > (DHCP_RENEW_TIMEOUT) )
  {
       Ethernet.maintain();
       last_dhcp_renew =  now;
  }
   // listen for incoming clients
  EthernetClient client = server.available();
  int pos, pos2;
  int buflength;
  String tempstr;
  String action_str;
  String qs_str;
  boolean qs = false;
  
  if (client) {
   // Serial.println("new client");
   
    // reset the input buffer
    bufstr = "";

    while (client.connected()) {
      if (client.available()) {
        char c = client.read();
        
        
        // If it isn't a new line, add the character to the buffer
        if (c != '\n' && c != '\r') {
         //read char by char HTTP request
          if (bufstr.length() < 100) {
          
          //store characters to string
          bufstr.concat(c);
          } 
                    
          // continue to read more data!
          continue;
        }

       
         if (bufstr.indexOf("GET / ") != -1) {
                // send a standard http response header
                client.println("HTTP/1.1 200 OK");
                client.println("Content-Type: text/html");
                client.println(); // empty line
                
                print_html_header(&client);
                //print_html_status(&client);
                print_html_footer(&client);
         }
          else if (bufstr.indexOf("GET /favicon.ico") != -1) {
             client.println("HTTP/1.0 404 Not Found");
          }
          else if (bufstr.indexOf("GET /robots.txt") != -1) { 
          
             client.println("HTTP/1.0 404 Not Found");
          }
        else if (bufstr.indexOf("GET /") != -1) {
          
                // Print it out for debugging
                //Serial.println(bufstr);
       
                buflength = bufstr.length();
                pos = bufstr.indexOf("/");
                pos2 = bufstr.indexOf("?", pos+1);
                qs = true;
                if(pos2 == -1)
                {
                   pos2 = bufstr.indexOf(" ", pos+1);
                   qs = false;
                }
                action_str = bufstr.substring(pos, pos2);            
                qs_str= "";
                if(qs)
                {
                  pos = bufstr.indexOf(" ", pos2+1);
                  qs_str = bufstr.substring(pos2+1, pos);
                  
                }

                  // send a standard http response header
                  client.println("HTTP/1.1 200 OK");
                  client.println("Content-Type: text/html");
                  client.println(); // empty line
                  print_html_header(&client);
                  process_action(&client, action_str, qs_str);
                  print_html_status(&client);
                  print_html_footer(&client);
                
           }
       
         break;
      }
    }
    // give the web browser time to receive the data
    delay(1);
    // close the connection:
    client.stop();
   // Serial.println("client disconnected");
  }
}

////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////
void test_button(int index, int up_down)
{
  if(shutters[index].buttons[up_down].pin == 0)
    return;

  int current_val =   digitalRead(shutters[index].buttons[up_down].pin);
  int current_relay = digitalRead(shutters[index].relays[up_down].pin);
  //int current_relay_op = digitalRead(shutters[index].relays[!up_down].pin);
  int next_relay_val;
  unsigned long inow = millis();
  
  // timeout management
  if(   ( (AUTO_STOP_TIMEOUT) > 0 )
     && (current_relay == RELAY_CLOSED )
     )
  {
          unsigned long diff = millis_diff(inow, shutters[index].last_action_time_ms);
          
          if(diff  > (AUTO_STOP_TIMEOUT) )
          { 
            ActivateRelay(index, up_down, RELAY_OPEN);
            return;
          }
        
  }
  
  boolean current_state = (current_relay == RELAY_CLOSED );
  if( current_state != shutters[index].relays[up_down].state)
  {
     trace_relay(index, up_down);
     Serial.println("state mismatch !!");
     shutters[index].relays[up_down].state = current_state;
  }
  
  if(!g_storage.enable_buttons)
  {
      return;
  }
  
  if (current_val == BUTTON_PRESSED)
  {

    if (shutters[index].last_action_button == -1 // first time (state is not correct)
        || shutters[index].last_action_button == (!up_down) // previous other button ?
        || !shutters[index].buttons[up_down].state)
    {       
      
      unsigned long diff = millis_diff(inow, shutters[index].last_action_time_ms);
      
        if(diff < MIN_TIME_ACTION_MS)
        {
           /* trace_button(index, up_down);
            Serial.print(" pushed too fast !! (");
            Serial.print(diff);
            Serial.println("ms)  ");*/
  
           /* Serial.print("last_action ");          
            Serial.print(shutters[index].last_action_button);    
            Serial.print(" state ");
            Serial.println(shutters[index].buttons[up_down].state);
      */
           return;
        }
      
    
      trace_button(index, up_down);
      Serial.print(" pushed (");
      Serial.print(diff);
      Serial.println("ms)");
     
   // always disable opposite relay
     ActivateRelay(index, !up_down, RELAY_OPEN);
     
     bool bnext_state = !(current_relay == RELAY_CLOSED);
     next_relay_val = bnext_state ? RELAY_CLOSED :  RELAY_OPEN;
     
     ActivateRelay(index, up_down, next_relay_val);
     
     shutters[index].last_action_time_ms = inow;

    }
    else
    {
        /*trace_button(index, up_down);
        Serial.println(" still pushed");*/
    }

    shutters[index].buttons[up_down].state = true;
    shutters[index].last_action_button = up_down;
  }
  else if (current_val == BUTTON_RELEASED)
  {
     if( shutters[index].buttons[up_down].state)
     {
        trace_button(index, up_down);
        Serial.println(" RELEASED");
      }
      
       //
    shutters[index].buttons[up_down].state = false;
  } 
  else
  {
      trace_button(index, up_down);
      Serial.println(" ??");
  }
}


////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////
void
test_general_button(int index, int up_down)
{
  if(shutters[index].buttons[up_down].pin == 0)
    return;

  int current_val =   digitalRead(shutters[index].buttons[up_down].pin);
  int current_relay = shutters[index].relays[up_down].state ? RELAY_CLOSED : RELAY_OPEN; //digitalRead(shutters[index].relays[up_down].pin);
  int current_relay_op = shutters[index].relays[!up_down].state? RELAY_CLOSED : RELAY_OPEN; //digitalRead(shutters[index].relays[!up_down].pin);
  unsigned long inow = millis();
  
  // timeout management
  if(   ( (AUTO_STOP_TIMEOUT) > 0 )
     && (current_relay == RELAY_CLOSED )
     )
  {
          unsigned long diff = millis_diff(inow, shutters[index].last_action_time_ms);
          
          if(diff  > (AUTO_STOP_TIMEOUT) )
          {     
            shutters[index].relays[up_down].state = false;
            shutters[index].relays[!up_down].state = false;
            trace_relay(index, up_down);
            Serial.print(" auto timeout (");
            Serial.print(diff);
            Serial.println("ms)  ");
            
            return;
          }
        
  }
  
  if(!g_storage.enable_centralized_buttons)
  {
      return;
  }
  
  if (current_val == BUTTON_PRESSED)
  {

    if (shutters[index].last_action_button == -1 
    ||  shutters[index].last_action_button == (!up_down)
    || (!shutters[index].buttons[up_down].state))
    {
      
      
    unsigned long diff = millis_diff(inow, shutters[index].last_action_time_ms);

      if(diff < MIN_TIME_ACTION_MS)
      {
          trace_button(index, up_down);
          Serial.print(" pushed too fast !! (");
          Serial.print(diff);
          Serial.println("ms)");
    
         return;
      }
    
    
      trace_button(index, up_down);
      Serial.print(" pushed (");
      Serial.print(diff);
      Serial.println("ms)");
      
      shutters[index].buttons[up_down].state = true;
   
      shutters[index].relays[up_down].state = !shutters[index].relays[up_down].state;
      shutters[index].relays[!up_down].state = false;
      
      for(int i=0; i<nbshutters; i++)
      {     
        shutters[i].relays[up_down].state = shutters[index].relays[up_down].state;
        shutters[i].relays[!up_down].state = false;
        
        digitalWrite (shutters[i].relays[!up_down].pin, RELAY_OPEN);
        digitalWrite (shutters[i].relays[up_down].pin, shutters[i].relays[up_down].state ? RELAY_CLOSED :  RELAY_OPEN);
      }
      
      shutters[index].last_action_time_ms = inow;
    }
   
    shutters[index].last_action_button = up_down;
  }
  else if (current_val == BUTTON_RELEASED)
  {
     if( shutters[index].buttons[up_down].state)
     {
        trace_button(index, up_down);
        Serial.println(" RELEASED");
      }
      
    shutters[index].buttons[up_down].state = false;
  }
  
}

////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////
//function VR ALL
void vrall(int index)
{ 
  test_general_button(index, ITEM_UP);
  test_general_button(index, ITEM_DOWN);
}

////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////
//function VR
void vr(int index)
{
   test_button(index, ITEM_UP);
   test_button(index, ITEM_DOWN);
}


////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////
void loop ()
{
   manage_client();
//    Serial.println("----------");

   
   for(int i=0; i<nbshutters; i++)
   {
         vr(i);
   }

   vrall(nbmaxitems-1); // -> VRALL ALL
  
  //delay(100);
  delay(10);
}
