////////////////////////////////////////////////////////////////////////////
// Rolling shutter management v0.1 (local, centralized and remote)
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
// Each shutter is connected with 3 wire (up/down/phase), if the phase
// is connected to 'up' it will go up, if connected to 'down' it will go down.
// Up and Down should not be connected at the same time
//
// For each shutter, I have 2 push-like buttons ("go up" "go down").
// If up is pressed once, the shutter should go up
// If up is pressed a second time, the shutter should stop
// Same logic for down.
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
#include <Ethernet2.h>
#include <EthernetServer.h>
#include <util.h>
#include <WString.h>

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
  
//////////////////////////
// how many shutters do you have ?
//////////////////////////
const int nbshutters = 10;

/////////////////////////
// Connected pins on your arduino
// REMARK: pins used by ethernet shield : 10 11 12 13
// ledUp and ledDown are for relays and pushButtonUp/pushButtonDown for buttons 
int        ledUp[] =   {30, 32, 34, 36,  38,  40,  42,  44,  46,  48,  0 };
int        ledDown[] = {31, 33, 34, 37,  39,  41,  43,  45,  47,  49,  0 };
int pushButtonUp[] =   { 4,  8, 14, 16,  18,  20,  22,  24,  26,  28,  50 };
int pushButtonDown[] = { 7,  9, 15, 17,  19,  21,  23,  25,  27,  29,  51 };

// you can give a name here that will be displayed on the webpage
const char* names[] = { "Ch Parents G", "Ch Parents D", 
                        "Ch enfants G", "Ch enfants D", 
                        "Ch invites", 
                        "Bureau", 
                        "Salon G", "Salon C", "Salon D", 
                        "Cuisine", "General" }; 


//////////////////////////
//  for button and relay
//////////////////////////
struct ITEM 
{ 
  int pin;
  boolean state;
  boolean last_state;
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
// the +1 is used for the centralized button
//////////////////////////
ROLLING_SHUTTER shutters[nbshutters+1];



// global flag to ignore hardware buttons and only use webpage API
boolean ignore_buttons = false;
boolean ignore_centralized_buttons = false;
 
//
String bufstr = String(100); //string for fetching data from address
int count = 0;
int g_debug = 0;

// Enter a MAC address and IP address for your controller below.
// The IP address will be dependent on your local network:
byte mac[] = {  0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED };

// Initialize the Ethernet server library
// with the IP address and port you want to use
// (port 80 is default for HTTP):
EthernetServer server(80);

////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////
void setup ()
{
  Serial.begin(9600);
   while (!Serial) {
    ; // wait for serial port to connect. Needed for Leonardo only
  }
  
  Serial.println("init");
  
  //init
  for(int i=0; i<nbshutters+1; i++)
  {
    shutters[i].buttons[ITEM_UP].pin = pushButtonUp[i];
    shutters[i].buttons[ITEM_UP].state = false;
    shutters[i].buttons[ITEM_UP].last_state = false;
    
    shutters[i].buttons[ITEM_DOWN].pin = pushButtonDown[i];
    shutters[i].buttons[ITEM_DOWN].state = false;
    shutters[i].buttons[ITEM_DOWN].last_state = false;
    
    shutters[i].relays[ITEM_UP].pin = ledUp[i];
    shutters[i].relays[ITEM_UP].state = false;
    shutters[i].relays[ITEM_UP].last_state = false;
    
    shutters[i].relays[ITEM_DOWN].pin = ledDown[i];
    shutters[i].relays[ITEM_DOWN].state = false;
    shutters[i].relays[ITEM_DOWN].last_state = false;
    
    shutters[i].last_action_button = -1;
    shutters[i].strname = names[i];
    shutters[i].last_action_time_ms = millis();
    
    if(i != nbshutters && shutters[i].relays[ITEM_UP].pin > 0)
    {
      pinMode (shutters[i].relays[ITEM_UP].pin, OUTPUT);
      pinMode (shutters[i].relays[ITEM_DOWN].pin, OUTPUT);
      
      digitalWrite(shutters[i].relays[ITEM_UP].pin, RELAY_OPEN);
      digitalWrite(shutters[i].relays[ITEM_DOWN].pin, RELAY_OPEN);
    }
    
    if( shutters[i].buttons[ITEM_UP].pin > 0)
    {
      pinMode (shutters[i].buttons[ITEM_UP].pin, INPUT);
      pinMode (shutters[i].buttons[ITEM_DOWN].pin, INPUT);
    }
  }
  
  Ethernet.begin(mac);
  server.begin();
  Serial.print("server is at ");
  Serial.println(Ethernet.localIP());
  
  Serial.println("init done");
}

////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////
void loop ()
{
   manage_client();
//    Serial.println("----------");

   if(ignore_buttons == false)
   for(int i=0; i<nbshutters; i++)
   {
      vr(i);
  }
 
  if(ignore_centralized_buttons == false)
  {
     vrall(nbshutters + 0); // -> VRALL ALL
  }
  //delay(100);
  delay(10);
}

////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////
void print_html_header(EthernetClient* client)
{
      client->println("<!DOCTYPE HTML>");
      client->println(" <STYLE type='text/css'>");
      client->println("* { margin:0 auto; padding:0; }");
      client->println("body { font-family:Tahoma,sans-serif,Verdana,Arial; color:black; }");
      client->println("table {border:1px solid black; border-collapse:collapse; padding: 10px;}");
      client->println("a,a:hover,a:visited {color:black; }");
      client->println("td {border: 1px solid black; padding: 3px; min-width:60px; }");
      client->println(" </STYLE>");
      client->println("<html><title>Shutters</title>");
}

////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////
void print_html_footer(EthernetClient* client)
{
       client->println("</html>");
}

////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////
void print_html_status(EthernetClient* client)
{
  client->print("<div>Buttons: ");

 if(ignore_buttons)
  client->print(" <a href='?ignorebuttons=false'>Disabled</a>");
 else
  client->print(" <a href='?ignorebuttons=true'>Enabled</a>");
  
  client->println("</div>");
   client->print("<div>Centralized buttons: ");
  if(ignore_centralized_buttons)
    client->print(" <a href='?ignorecentralizedbuttons=false'>Disabled</a>");
  else
    client->print(" <a href='?ignorecentralizedbuttons=true'>Enabled</a>");
  client->println("</div>");
  
      client->println("<table><tr><td>shutter</td><td>action</td><td>state</td><td>button up</td><td>button down</td><td>relay up</td><td>relay down</td></tr>");
      // output the value of each analog input pin
      for (int i = 0; i < nbshutters+1; i++) 
      {
        if( shutters[i].buttons[ITEM_UP].pin == 0)
          continue;
          
        boolean sread_up = shutters[i].relays[ITEM_UP].state;
        boolean sread_down = shutters[i].relays[ITEM_DOWN].state;
        client->println("<tr><td>");
        client->print(shutters[i].strname);

     client->print("</td><td>");
         client->print("<a href='?");
            client->print(i+1);
            client->print("=up'>UP</a>");
         client->print(" <a href='?");
            client->print(i+1);
            client->print("=stop'>STOP</a>");
            client->print(" <a href='?");
            client->print(i+1);
            client->print("=down'>DOWN</a>");
        client->println("</td><td>");
        if(sread_up  && !sread_down)
        {
            client->print("UP");
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
        client->print("</td><td>(");
        client->print(shutters[i].buttons[ITEM_UP].pin);
        client->print(") ");
        client->print(shutters[i].buttons[ITEM_UP].state);
    
        client->print("</td><td>(");
        client->print(shutters[i].buttons[ITEM_DOWN].pin);
        client->print(") ");
        client->print(shutters[i].buttons[ITEM_DOWN].state);
        
         client->print("</td><td>(");
        client->print(shutters[i].relays[ITEM_UP].pin);
        client->print(") ");
        client->print(shutters[i].relays[ITEM_UP].state);
        
        client->print("</td><td>(");
        client->print(shutters[i].relays[ITEM_DOWN].pin);
        client->print(") ");
        client->print(shutters[i].relays[ITEM_DOWN].state);
        
        
       client->println("</td></tr>");
      }
      client->println("</table>");
}

////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////
void process_shutter_query(EthernetClient* client, String query)
{
    int sep = 0;

    while(sep != -1)
    {
      int sep2 = query.indexOf("&", sep);
      String strparam = query.substring(sep, sep2);
      if(strparam.length() < 3)
      {
        if(sep2 == -1) break;
        sep= sep2+1;
        continue;
      }
        
      int equ = strparam.indexOf("=");
      if(equ != -1)
      {
          String strname = strparam.substring(0, equ);
          String strvalue = strparam.substring(equ+1);
          
          /*client->print("<div>PARAM ");
          client->print(strname);
          client->print(" VALUE ");
          client->print(strvalue);
          client->println("</div>");*/
          
          int iv = strname.toInt() - 1;
          
          if(strname == "ignorebuttons")
          {
             if(strvalue == "true")
             {            
               ignore_buttons= true;
             }
             else  if(strvalue == "false")
             {            
               ignore_buttons= false;
             }
          }
          else if(strname == "ignorecentralizedbuttons")
          {
            if(strvalue == "true")
             {            
               ignore_centralized_buttons= true;
             }
             else  if(strvalue == "false")
             {            
               ignore_centralized_buttons= false;
             }
          }
          else if(strname == "all" || iv == nbshutters)
          {
             if(strvalue == "stop")
            {
                for(int i = 0; i < nbshutters; i++)
                {
                  shutters[i].relays[ITEM_UP].state = false;
                  shutters[i].relays[ITEM_DOWN].state = false;
                  digitalWrite(shutters[i].relays[ITEM_DOWN].pin, RELAY_OPEN);
                  digitalWrite(shutters[i].relays[ITEM_UP].pin, RELAY_OPEN);
                }
                shutters[nbshutters].relays[ITEM_UP].state = false;
                shutters[nbshutters].relays[ITEM_DOWN].state = false;
                  
              if(g_debug) client->println("<div>all stop</div>");
            }
            else if(strvalue == "up")
            {
                for(int i = 0; i < nbshutters; i++)
                {
                  shutters[i].relays[ITEM_UP].state = true;
                  shutters[i].relays[ITEM_DOWN].state = false;
                  digitalWrite(shutters[i].relays[ITEM_DOWN].pin, RELAY_OPEN);
                  digitalWrite(shutters[i].relays[ITEM_UP].pin, RELAY_CLOSED);
                }
                shutters[nbshutters].relays[ITEM_UP].state = true;
                shutters[nbshutters].relays[ITEM_DOWN].state = false;
                
              if(g_debug) client->println("<div>all up</div>");
            }
            else if(strvalue == "down")
            {
                for(int i = 0; i < nbshutters; i++)
                {
                  shutters[i].relays[ITEM_UP].state = false;
                  shutters[i].relays[ITEM_DOWN].state = true;
                  digitalWrite(shutters[i].relays[ITEM_UP].pin, RELAY_OPEN);
                  digitalWrite(shutters[i].relays[ITEM_DOWN].pin, RELAY_CLOSED);
                }
                
                shutters[nbshutters].relays[ITEM_UP].state = false;
                shutters[nbshutters].relays[ITEM_DOWN].state = true;
                
              if(g_debug)  client->println("<div>all down</div>");
            }
          }
          
          
          if(iv < 0 || iv >= nbshutters)
          {
            if(sep2 == -1) break;
            sep= sep2+1;
            continue;
          }
            
          if(strvalue == "stop")
          {
              shutters[iv].relays[ITEM_UP].state = false;
              shutters[iv].relays[ITEM_DOWN].state = false;
              digitalWrite(shutters[iv].relays[ITEM_DOWN].pin, RELAY_OPEN);
              digitalWrite(shutters[iv].relays[ITEM_UP].pin, RELAY_OPEN);
              if(g_debug) client->println("<div>stop</div>");
          }
          else if(strvalue == "up")
          {
              shutters[iv].relays[ITEM_UP].state = true;
              shutters[iv].relays[ITEM_DOWN].state = false;
              digitalWrite(shutters[iv].relays[ITEM_DOWN].pin, RELAY_OPEN);
              digitalWrite(shutters[iv].relays[ITEM_UP].pin, RELAY_CLOSED);
              if(g_debug) client->println("<div>up</div>");
          }
          else if(strvalue == "down")
          {
              shutters[iv].relays[ITEM_UP].state = false;
              shutters[iv].relays[ITEM_DOWN].state = true;
              digitalWrite(shutters[iv].relays[ITEM_UP].pin, RELAY_OPEN);
              digitalWrite(shutters[iv].relays[ITEM_DOWN].pin, RELAY_CLOSED);
              if(g_debug) client->println("<div>down</div>");
          }
      }
      
      if(sep2 == -1) break;
      sep= sep2+1;
    }
    
}

////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////
void process_action(EthernetClient* client, String action, String query)
{
    if(action == "/shutter")
    {
        process_shutter_query(client, query);
    }
}

////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////
void manage_client()
{
   // listen for incoming clients
  EthernetClient client = server.available();
  int pos, pos2;
  int buflength;
  String tempstr;
  String action_str;
  String qs_str;
  boolean qs = false;
  
  if (client) {
    Serial.println("new client");
   
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

        
        // Print it out for debugging
        Serial.println(bufstr);
        
         if (bufstr.indexOf("GET / ") != -1) {
                // send a standard http response header
                client.println("HTTP/1.1 200 OK");
                client.println("Content-Type: text/html");
                client.println(); // empty line
                
                print_html_header(&client);
                //print_html_status(&client);
                print_html_footer(&client);
         }
        else if (bufstr.indexOf("GET /") != -1) {
                
                // send a standard http response header
                client.println("HTTP/1.1 200 OK");
                client.println("Content-Type: text/html");
                client.println(); // empty line
                 print_html_header(&client);
                
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
                
                /*client.print("URL: ");
                client.print(action_str);*/
                
                qs_str= "";
                if(qs)
                {
                  pos = bufstr.indexOf(" ", pos2+1);
                  
                  qs_str = bufstr.substring(pos2+1, pos);
                  
                 /* client.print("<br>QueryString: ");
                  client.print(qs_str);
                  client.print("<br>");*/
                }
                
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
    Serial.println("client disconnected");
  }
}


////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////
void 
DW(int pin, int val)
{
     /* Serial.print("pin  ");
      Serial.print(pin);
      Serial.print(" -> ");
      Serial.println(val);*/
      
      digitalWrite (pin, val);
}

////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////
void
trace_button(int index, int up_down)
{
      Serial.print("[");
      Serial.print(count);
      Serial.print("]");
      Serial.print(" button ");
      Serial.print(index);
      Serial.print(":");
      Serial.print(up_down == ITEM_UP ? "UP" : "DOWN");
      Serial.print(" (");
      Serial.print(shutters[index].buttons[up_down].pin);
      Serial.print(")");
      count++;
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
  
  if (current_val == BUTTON_PRESSED)
  {

    if (shutters[index].last_action_button == -1 // first time (last_state is not correct)
        || shutters[index].last_action_button == (!up_down) // previous other button ?
        || !shutters[index].buttons[up_down].last_state)
    {       
      
      int diff = 0; 
    if(millis() >= shutters[index].last_action_time_ms)
    {
      diff = millis() - shutters[index].last_action_time_ms;
      if(diff < MIN_TIME_ACTION_MS)
      {
          trace_button(index, up_down);
          Serial.print(" pushed too fast !! (");
          Serial.print(diff);
          Serial.println("ms)");
    
         return;
      }
    }
    
      trace_button(index, up_down);
      Serial.print(" pushed (");
      Serial.print(diff);
      Serial.println("ms)");
      
    shutters[index].relays[up_down].state = !(current_relay == RELAY_CLOSED);

   // always disable opposite relay
     // if(current_relay_op != RELAY_OPEN)
    DW(shutters[index].relays[!up_down].pin, RELAY_OPEN);
         
     next_relay_val = shutters[index].relays[up_down].state ? RELAY_CLOSED :  RELAY_OPEN;
     if(current_relay != next_relay_val)
        DW (shutters[index].relays[up_down].pin, next_relay_val);
        
     shutters[index].last_action_time_ms = millis();
    }

    shutters[index].buttons[up_down].state = true;
    shutters[index].last_action_button = up_down;
    shutters[index].buttons[up_down].last_state = true;
  }
  else if (current_val == BUTTON_RELEASED)
  {
    shutters[index].buttons[up_down].state = false;
   
     if( shutters[index].buttons[up_down].last_state)
     {
        trace_button(index, up_down);
        Serial.println(" RELEASED");
      }
      
       //
    shutters[index].buttons[up_down].last_state = false;
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
  int current_relay = digitalRead(shutters[index].relays[up_down].pin);
  int current_relay_op = digitalRead(shutters[index].relays[!up_down].pin);
  
  if (current_val == BUTTON_PRESSED)
  {

    if (shutters[index].last_action_button == -1 
    ||  shutters[index].last_action_button == (!up_down)
    || (!shutters[index].buttons[up_down].last_state))
    {
      
      
    int diff = 0; 
    if(millis() >= shutters[index].last_action_time_ms)
    {
      diff = millis() - shutters[index].last_action_time_ms;
      if(diff < MIN_TIME_ACTION_MS)
      {
          trace_button(index, up_down);
          Serial.print(" pushed too fast !! (");
          Serial.print(diff);
          Serial.println("ms)");
    
         return;
      }
    }
    
      trace_button(index, up_down);
      Serial.print(" pushed (");
      Serial.print(diff);
      Serial.println("ms)");
      
      shutters[index].buttons[up_down].state = true;
   
      shutters[index].relays[up_down].state = !shutters[index].relays[up_down].state;
      shutters[index].relays[!up_down].state = false;
      
      shutters[index].buttons[up_down].last_state = true;
      for(int i=0; i<nbshutters; i++)
      {     
        shutters[i].relays[up_down].state = shutters[index].relays[up_down].state;
        shutters[i].relays[!up_down].state = false;
        
        digitalWrite (shutters[i].relays[!up_down].pin, RELAY_OPEN);
        digitalWrite (shutters[i].relays[up_down].pin, shutters[i].relays[up_down].state ? RELAY_CLOSED :  RELAY_OPEN);
      }
      
      shutters[index].last_action_time_ms = millis();
    }
   
    shutters[index].last_action_button = up_down;
  }
  else if (current_val == BUTTON_RELEASED)
  {
     shutters[index].buttons[up_down].state = false;
   
     if( shutters[index].buttons[up_down].last_state)
     {
        trace_button(index, up_down);
        Serial.println(" RELEASED");
      }
      
    shutters[index].buttons[up_down].last_state = false;
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

