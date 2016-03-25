//--------------------------------------------------------------------------------
// ED BMSdiag, v0.24
// Retrieve battery diagnostic data from your smart electric drive EV.
//
// (c) 2016 by MyLab-odyssey
//
// Licensed under "MIT License (MIT)", see license file for more information.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDER OR CONTRIBUTORS "AS IS" AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
// ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
// (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
// LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
// ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//--------------------------------------------------------------------------------
//! \file    ED_BMSdiag.ino
//! \brief   Retrieve battery diagnostic data from your smart electric drive EV.
//! \brief   Build a diagnostic tool with the MCP2515 CAN controller and Arduino
//! \brief   compatible hardware.
//! \date    2016-March
//! \author  My-Lab-odyssey
//! \version 0.24
//--------------------------------------------------------------------------------

//#define DO_DEBUG_UPDATE        //!< Uncomment to show DEBUG output
#define RAW_VOLTAGES 0           //!< Use RAW values or calc ADC offset voltage
#define VERBOSE 1                //!< VERBOSE mode will output individual cell data

#ifndef DO_DEBUG_UPDATE
#define DEBUG_UPDATE(...)
#else
#define DEBUG_UPDATE(...) Serial.print(__VA_ARGS__)
#endif

#include <mcp_can.h>
#include <SPI.h>
#include <Timeout.h>
#include <Average.h>

//Global definitions
#define DATALENGTH 440
#define CELLCOUNT 93
#define SPACER F("-----------------------------------------")
#define MSG_OK F("OK")
#define MSG_FAIL F("FAIL")

//Data arrays
unsigned int data[DATALENGTH]; 
Average <unsigned int> CellVoltage(CELLCOUNT);
Average <unsigned int> CellCapacity(CELLCOUNT);

//BMS data structure
typedef struct {
  //
  unsigned int ADCCvolts_min;    //!< minimum cell voltage in mV, add offset +1500
  unsigned int ADCCvolts_mean;   //!< average cell voltage in mV, no offset
  unsigned int ADCCvolts_max;    //!< maxiimum cell voltage in mV, add offset +1500
  int ADCvoltsOffset;            //!< calculated offset between RAW cell voltages and ADCref, about 90mV
  unsigned int Cap_min_As;       //!< mininum cell capacity from measurement cycle
  unsigned int Cap_mean_As;      //!< average cell capacity from measurement cycle
  unsigned int Cap_max_As;       //!< maximum cell capacity from measurement cycle
  
  unsigned int Cvolts_min;       //!< voltage mininum in pack
  float Cvolts_mean;             //!< calculated average from individual cell voltage query
  unsigned int Cvolts_max;       //!< voltage maxinum in pack
  float Cvolts_stdev;            //!< calculated standard deviation (populated)
  unsigned int Ccap_min_As;      //!< cell capacity mininum in pack
  unsigned int Ccap_mean_As;     //!< cell capacity calculated average
  unsigned int Ccap_max_As;      //!< cell capacity maxinum in pack
  
  unsigned long HVoff_time;      //!< HighVoltage contactor off time in seconds
  float Cap_meas_quality;        //!< some sort of estimation factor??? after measurement cycle
  float Cap_combined_quality;    //!< some sort of estimation factor??? constantly updated
  int LastMeas_days;             //!< days elapsed since last successful measurement
  
  float SOC;                     //!< State of Charge, as reported by vehicle dash
  float HV;                      //!< total voltage of HV system in V
  
  unsigned int HVcontactState;   //!< contactor state: 0 := OFF, 2 := ON
  long HVcontactCyclesLeft;      //!< counter related to ON/OFF cyles of the car
  long HVcontactCyclesMax;       //!< static, seems to be maxiumum of contactor cycles
  
} BatteryDiag_t; 

//CAN-Bus declarations
long unsigned int rxID;
unsigned char len = 0;
unsigned char rxLength = 0;
unsigned char rxBuf[8];
unsigned char rqInit[8] = {0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0};
unsigned char rqFlowControl[8] = {0x30, 0x08, 0x14, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
byte rqFC_length = 8;            //!< Interval to send flow control messages (rqFC) 
unsigned char rqBattADCref[8] = {0x03, 0x22, 0x02, 0x07, 0xFF, 0xFF, 0xFF, 0xFF};
unsigned char rqBattVolts[8] = {0x03, 0x22, 0x02, 0x08, 0xFF, 0xFF, 0xFF, 0xFF};
unsigned char rqBattCapacity[8] = {0x03, 0x22, 0x03, 0x10, 0xFF, 0xFF, 0xFF, 0xFF};
unsigned char rqBattHVContactorCyclesLeft[8] = {0x03, 0x22, 0x03, 0x0B, 0xFF, 0xFF, 0xFF, 0xFF};
unsigned char rqBattHVContactorMax[8] = {0x03, 0x22, 0x03, 0x0C, 0xFF, 0xFF, 0xFF, 0xFF};
unsigned char rqBattHVContactorState[8] = {0x03, 0x22, 0xD0, 0x00, 0xFF, 0xFF, 0xFF, 0xFF};

#define CS     10                //!< chip select pin of MCP2515 CAN-Controller
#define CS_SD  8                 //!< CS for SD card, if you plan to use a logger...
MCP_CAN CAN0(CS);                // Set CS pin

BatteryDiag_t BattDiag;
CTimeout CAN_Timeout(10000);     //!< Timeout value for CAN response in millis

//--------------------------------------------------------------------------------
//! \brief   SETUP()
//--------------------------------------------------------------------------------
void setup()
{  
  Serial.begin(115200);
  while (!Serial); // while the serial stream is not open, do nothing:
  Serial.println();
  
  pinMode(CS, OUTPUT);
  pinMode(CS_SD, OUTPUT);
  digitalWrite(CS_SD, HIGH);
  
  // Initialize MCP2515 running at 16MHz with a baudrate of 500kb/s and the masks and filters enabled.
  if(CAN0.begin(MCP_STD, CAN_500KBPS, MCP_16MHZ) == CAN_OK) DEBUG_UPDATE(F("MCP2515 Init Okay!!\r\n"));
  else DEBUG_UPDATE(F("MCP2515 Init Failed!!\r\n"));
  
  clearCAN_Filter();
    
  digitalWrite(CS, HIGH);
  
  // MCP2515 read buffer: setting pin 2 for input, LOW if CAN messages are received
  pinMode(2, INPUT); 
  
  Serial.println(SPACER); 
  Serial.println(F("--- ED Battery Management Diagnostics ---"));
  Serial.println(SPACER);
  
  Serial.println(F("Connect to OBD port - Waiting for CAN-Bus "));
  do {
    Serial.print(F("."));
    delay(1000);
  } while (digitalRead(2));
  Serial.println(F("CONNECTED"));
}

//--------------------------------------------------------------------------------
//! \brief   Wait for serial data to be avaiable.
//--------------------------------------------------------------------------------
void WaitforSerial() {
  Serial.println(F("Press a key to start query..."));
  while (!Serial.available()) {}                  // Wait for serial input to start
}

//--------------------------------------------------------------------------------
//! \brief   Read all queued charachers to clear input buffer.
//--------------------------------------------------------------------------------
void clearSerialBuffer() {
  do {                                            // Clear serial input buffer
      delay(10);
  } while (Serial.read() >= 0);
}

//--------------------------------------------------------------------------------
//! \brief   Clear CAN ID filters.
//--------------------------------------------------------------------------------
void clearCAN_Filter(){
  CAN0.init_Mask(0, 0, 0x00000000);
  CAN0.init_Mask(1, 0, 0x00000000);
  delay(100);
  CAN0.setMode(MCP_NORMAL);                     // Set operation mode to normal so the MCP2515 sends acks to received data.
}

//--------------------------------------------------------------------------------
//! \brief   Set all filters to one CAN ID.
//--------------------------------------------------------------------------------
void setCAN_Filter(unsigned long filter){
  filter = filter << 16;
  CAN0.init_Mask(0, 0, 0x07FF0000);
  CAN0.init_Mask(1, 0, 0x07FF0000);
  CAN0.init_Filt(0, 0, filter);
  CAN0.init_Filt(1, 0, filter);
  CAN0.init_Filt(2, 0, filter);
  CAN0.init_Filt(3, 0, filter);
  CAN0.init_Filt(4, 0, filter);
  CAN0.init_Filt(5, 0, filter);
  delay(100);
  CAN0.setMode(MCP_NORMAL);                     // Set operation mode to normal so the MCP2515 sends acks to received data.
}

//--------------------------------------------------------------------------------
//! \brief   Send diagnostic request to ECU.
//! \param   unsigned char* rqQuery
//! \see     rqInit, rqBattADCref ... rqBattVolts
//! \return  received items count (unsigned int) of function #Get_RequestResponse
//--------------------------------------------------------------------------------
unsigned int Request_Diagnostics(unsigned char* rqQuery){  
  CAN_Timeout.Reset();                          // Reset Timeout-Timer
  
  digitalWrite(CS_SD, HIGH);                    // Disable SD card, or other SPI devices if nessesary
  
  //--- Diag Request Message ---
  DEBUG_UPDATE(F("Send Diag Request\n\r"));
  CAN0.sendMsgBuf(0x7E7, 0, 8, rqQuery);        // send data: Request diagnostics data
  
  return Get_RequestResponse();                 // wait for response of first frame
}

//--------------------------------------------------------------------------------
//! \brief   Wait and read initial diagnostic response
//! \return  item count (unsigned int)
//--------------------------------------------------------------------------------
unsigned int Get_RequestResponse(){ 
    
    int i;
    unsigned int items = 0;
    byte FC_length = rqFlowControl[1];    
    boolean fDataOK = false;
    
    do{
      //--- Read Frames ---
      if(!digitalRead(2))                         // If pin 2 is LOW, read receive buffer
      {
        do{
          CAN0.readMsgBuf(&rxID, &len, rxBuf);    // Read data: len = data length, buf = data byte(s)       
          
          if (rxID == 0x7EF) { 
            if(rxBuf[0] < 0x10) {
              if((rxBuf[1] != 0x7F)) {  
                for (i = 0; i<len; i++) {         // read data bytes: offset +1, 1 to 7
                    data[i] = rxBuf[i+1];       
                }
                DEBUG_UPDATE(F("SF reponse: "));
                DEBUG_UPDATE(rxBuf[0] & 0x0F); DEBUG_UPDATE("\n\r");
                items = 0;
                fDataOK = true;
              } else if (rxBuf[3] == 0x78) {
                DEBUG_UPDATE(F("pending reponse...\n\r"));
              } else {
                DEBUG_UPDATE(F("ERROR\n\r"));
              }
            }
            if ((rxBuf[0] & 0xF0) == 0x10){
              items = (rxBuf[0] & 0x0F)*256 + rxBuf[1]; // six data bytes already read (+ two type and length)
              for (i = 0; i<len; i++) {                 // read data bytes: offset +1, 1 to 7
                  data[i] = rxBuf[i+1];       
              }
              //--- send rqFC: Request for more data ---
              CAN0.sendMsgBuf(0x7E7, 0, 8, rqFlowControl);
              DEBUG_UPDATE(F("Resp, i:"));
              DEBUG_UPDATE(items - 6); DEBUG_UPDATE("\n\r");
              fDataOK = Read_FC_Response(items - 6);
            } 
          }     
        } while(!digitalRead(2) && !CAN_Timeout.Expired(false) && !fDataOK);
      }
    } while (!CAN_Timeout.Expired(false) && !fDataOK);

    if (fDataOK) {
      return (items + 7) / 7;
      DEBUG_UPDATE(F("success!\n\r"));
    } else {
      DEBUG_UPDATE(F("Event Timeout!\n\r")); 
      ClearReadBuffer(); 
      return 0; 
    } 
}

//--------------------------------------------------------------------------------
//! \brief   Read remaining data and sent corresponding Flow Control frames
//! \param   items still to read (int)
//! \return  fDiagOK (boolean)
//--------------------------------------------------------------------------------
boolean Read_FC_Response(int items){   
    CAN_Timeout.Reset();
    
    int i;
    int n = 7;
    int FC_count = 0;
    byte FC_length = rqFlowControl[1];
    boolean fDiagOK = false;
    
    do{
      //--- Read Frames ---
      if(!digitalRead(2))                         // If pin 2 is LOW, read receive buffer
      {
        do{
          CAN0.readMsgBuf(&rxID, &len, rxBuf);    // Read data: len = data length, buf = data byte(s)       
          if((rxBuf[0] & 0xF0) == 0x20){
            FC_count++;
            items = items - len + 1;
            for(i = 0; i<len; i++) {              // copy each byte of the rxBuffer to data-field
              if ((n < (DATALENGTH - 6)) && (i < 7)){
                data[n+i] = rxBuf[i+1];
              }       
            }
            //--- FC counter -> then send Flow Control Message ---
            if (FC_count % FC_length == 0 && items > 0) {
              // send rqFC: Request for more data
              CAN0.sendMsgBuf(0x7E7, 0, 8, rqFlowControl);
              DEBUG_UPDATE(F("FCrq\n\r"));
            }
            n = n + 7;
          }      
        } while(!digitalRead(2) && !CAN_Timeout.Expired(false) && items > 0);
      }
    } while (!CAN_Timeout.Expired(false) && items > 0);
    if (!CAN_Timeout.Expired(false)) {
      fDiagOK = true;
      DEBUG_UPDATE(F("Items left: ")); DEBUG_UPDATE(items); DEBUG_UPDATE("\n\r");
      DEBUG_UPDATE(F("FC count: ")); DEBUG_UPDATE(FC_count); DEBUG_UPDATE("\n\r");
    } else {
      fDiagOK = false;
      DEBUG_UPDATE(F("Event Timeout!\n\r"));
    } 
    ClearReadBuffer();    
    return fDiagOK;
}

//--------------------------------------------------------------------------------
//! \brief   Output read buffer
//! \param   items count (unsigned int)
//--------------------------------------------------------------------------------
void PrintReadBuffer(unsigned int items) {
  Serial.println(items);
  for(int i = 0; i < items; i++) {
      Serial.print(F("Data: "));
      for(int n = 0; n < 7; n++)               // Print each byte of the data.
      {
        if(data[n + 7 * i] < 0x10)             // If data byte is less than 0x10, add a leading zero.
        {
          Serial.print(F("0"));
        }
        Serial.print(data[n + 7 * i], HEX);
        Serial.print(" ");
      }
      Serial.println();
  }
}

//--------------------------------------------------------------------------------
//! \brief   Cleanup after switcheng filters
//--------------------------------------------------------------------------------
void ClearReadBuffer(){
  if(!digitalRead(2)) {                        // still messages? pin 2 is LOW, clear the two rxBuffers by reading
    for (int i = 1; i <= 2; i++) {
      CAN0.readMsgBuf(&rxID, &len, rxBuf);
    }
    DEBUG_UPDATE(F("Buffer cleared!\n\r"));
  }
}

//--------------------------------------------------------------------------------
//! \brief   Store two byte data in CellCapacity obj
//--------------------------------------------------------------------------------
void ReadCellCapacity(unsigned int data_in[], unsigned int highOffset, unsigned int length){
  for(int n = 0; n < (length * 2); n = n + 2){
    CellCapacity.push((data_in[n + highOffset] * 256 + data_in[n + highOffset + 1]));
  }
}

//--------------------------------------------------------------------------------
//! \brief   Store two byte data in CellVoltage obj
//--------------------------------------------------------------------------------
void ReadCellVoltage(unsigned int data_in[], unsigned int highOffset, unsigned int length){
  for(int n = 0; n < (length * 2); n = n + 2){
    CellVoltage.push((data_in[n + highOffset] * 256 + data_in[n + highOffset + 1]));
  }
}

//--------------------------------------------------------------------------------
//! \brief   Store two byte data
//! \param   address to output data array (unsigned int)
//! \param   address to input data array (unsigned int)
//! \param   start of first high byte in data array (unsigned int)
//! \param   length of data submitted (unsigned int)
//--------------------------------------------------------------------------------
void ReadDiagWord(unsigned int data_out[], unsigned int data_in[], unsigned int highOffset, unsigned int length){
  for(int n = 0; n < (length * 2); n = n + 2){
    data_out[n/2] = data_in[n + highOffset] * 256 + data_in[n + highOffset + 1];
  }
}

//--------------------------------------------------------------------------------
//! \brief   Read and evaluate capacity data
//! \param   enable verbose / debug output (boolean)
//! \return  report success (boolean)
//--------------------------------------------------------------------------------
boolean getBatteryCapacity(boolean debug_verbose) {
  unsigned int items = Request_Diagnostics(rqBattCapacity);
  if(items){
    if (debug_verbose) {
      PrintReadBuffer(items);
    }   
    CellCapacity.clear();
    ReadCellCapacity(data,25,CELLCOUNT);
    BattDiag.Ccap_min_As = CellCapacity.minimum();
    BattDiag.Ccap_max_As = CellCapacity.maximum();
    BattDiag.Ccap_mean_As = CellCapacity.mean();

    BattDiag.HVoff_time = (unsigned long) data[5] * 65535 + data[6] * 256 + data[7];
    ReadDiagWord(&BattDiag.Cap_min_As,data,21,1);
    ReadDiagWord(&BattDiag.Cap_mean_As,data,23,1);
    ReadDiagWord(&BattDiag.Cap_max_As,data,17,1);
    BattDiag.LastMeas_days = data[428];
    unsigned int value;
    ReadDiagWord(&value,data,429,1);
    BattDiag.Cap_meas_quality = value / 65535.0;
    ReadDiagWord(&value,data,425,1);
    BattDiag.Cap_combined_quality = value / 65535.0;
    return true;
  } else {
    return false;
  }
}

//--------------------------------------------------------------------------------
//! \brief   Read and evaluate voltage data
//! \param   enable verbose / debug output (boolean)
//! \return  report success (boolean)
//--------------------------------------------------------------------------------
boolean getBatteryVoltage(boolean debug_verbose) {
  unsigned int items;
  items = Request_Diagnostics(rqBattVolts);
  if(items){
    if (debug_verbose) {
      PrintReadBuffer(items);
    }   
    CellVoltage.clear();
    ReadCellVoltage(data,4,CELLCOUNT);
    BattDiag.Cvolts_min = CellVoltage.minimum();
    BattDiag.Cvolts_max = CellVoltage.maximum();
    BattDiag.Cvolts_mean = CellVoltage.mean();
    BattDiag.Cvolts_stdev = CellVoltage.stddev();
    return true;
  } else {
    return false;
  }
}

//--------------------------------------------------------------------------------
//! \brief   Read and evaluate ADC reference data
//! \param   enable verbose / debug output (boolean)
//! \return  report success (boolean)
//--------------------------------------------------------------------------------
boolean getBatteryADCref(boolean debug_verbose) {
  unsigned int items;
  items = Request_Diagnostics(rqBattADCref);
  if(items){
    if (debug_verbose) {
      PrintReadBuffer(items);
    }   
    ReadDiagWord(&BattDiag.ADCCvolts_mean,data,8,1);
    
    ReadDiagWord(&BattDiag.ADCCvolts_min,data,6,1);
    BattDiag.ADCCvolts_min += 1500;
    ReadDiagWord(&BattDiag.ADCCvolts_max,data,4,1);
    BattDiag.ADCCvolts_max += 1500;
    
    if (RAW_VOLTAGES) {
      BattDiag.ADCvoltsOffset = 0;
    } else {
      BattDiag.ADCvoltsOffset = (int) BattDiag.Cvolts_mean - BattDiag.ADCCvolts_mean;
    }
    
    return true;
  } else {
    return false;
  }
}

//--------------------------------------------------------------------------------
//! \brief   Read and evaluate High Voltage contractor state
//! \param   enable verbose / debug output (boolean)
//! \return  report success (boolean)
//--------------------------------------------------------------------------------
boolean getHVcontactorState(boolean debug_verbose) {
  unsigned int items;
  boolean fValid = false;
  items = Request_Diagnostics(rqBattHVContactorCyclesLeft);
  if(items){
    if (debug_verbose) {
      PrintReadBuffer(items);
    }   
    BattDiag.HVcontactCyclesLeft = (long) data[4] * 65536 + data[5] * 256 + data[6]; 
    fValid = true;
  } else {
    fValid = false;
  }
  items = Request_Diagnostics(rqBattHVContactorMax);
  if(items){
    if (debug_verbose) {
      PrintReadBuffer(items);
    }   
    BattDiag.HVcontactCyclesMax = (long) data[4] * 65536 + data[5] * 256 + data[6]; 
    fValid = true;
  } else {
    fValid = false;
  }
  items = Request_Diagnostics(rqBattHVContactorState);
  if(items){
    if (debug_verbose) {
      PrintReadBuffer(items);
    }   
    BattDiag.HVcontactState = (unsigned int) data[3]; 
    fValid = true;
  } else {
    fValid = false;
  }
  return fValid;
}

//--------------------------------------------------------------------------------
//! \brief   Read and evaluate SOC
//! \return  report success (boolean)
//--------------------------------------------------------------------------------
boolean ReadSOC() {
  setCAN_Filter(0x518);
  CAN_Timeout.Reset();
  
  if(!digitalRead(2)) {    
    do {    
    CAN0.readMsgBuf(&rxID, &len, rxBuf); 
      if (rxID == 0x518 ) {
        BattDiag.SOC = (float) rxBuf[7] / 2;
        return true;
      }
    } while (!CAN_Timeout.Expired(false));
  }
  return false;
}

//--------------------------------------------------------------------------------
//! \brief   Read and evaluate system High Voltage
//! \return  report success (boolean)
//--------------------------------------------------------------------------------
boolean ReadHV() {
  setCAN_Filter(0x448);
  CAN_Timeout.Reset();
  
  float HV;
  if(!digitalRead(2)) {    
    do {    
    CAN0.readMsgBuf(&rxID, &len, rxBuf); 
      if (rxID == 0x448 ) {
        HV = ((float)rxBuf[6]*256 + (float)rxBuf[7]);
        HV = HV / 10.0;
        BattDiag.HV = HV;
        return true;
      }
    } while (!CAN_Timeout.Expired(false));
  }
  return false;
}

//--------------------------------------------------------------------------------
//! \brief   LOOP()
//--------------------------------------------------------------------------------
void loop()
{       
   boolean fOK = false;
   
   //Wait for start via serial terminal
   WaitforSerial();
   clearSerialBuffer();
   Serial.println(SPACER);
   delay(500);
     
   fOK = ReadSOC();
   fOK = ReadHV();
   
   setCAN_Filter(0x7EF); 
   int testStep = 0;
   do {
      switch (testStep) {
        case 0:
           Serial.print(F("Read Voltages..."));
           if (fOK = getBatteryVoltage(false)) {
             Serial.println(MSG_OK);
           } else {
             Serial.println(MSG_FAIL);
           }
           break;
        case 1:
           Serial.print(F("Read Capacity..."));
           if (fOK = getBatteryCapacity(false)) {
             Serial.println(MSG_OK);
           } else {
             Serial.println(MSG_FAIL);
           }
           break;
        case 2:
           Serial.print(F("Read ADCref. ..."));
           if (fOK = getBatteryADCref(false)) {
             Serial.println(MSG_OK);
           } else {
             Serial.println(MSG_FAIL);
           }
           break;
        case 3:
           Serial.print(F("Read HV state..."));
           if (fOK = getHVcontactorState(false)) {
             Serial.println(MSG_OK);
           } else {
             Serial.println(MSG_FAIL);
           }
           break;
      }
      testStep++;
   } while (fOK && testStep < 4);
    
   if (fOK) {
      digitalWrite(CS, HIGH);
      Serial.println(SPACER);
      Serial.print(F("SOC     : ")); Serial.print(BattDiag.SOC,1);
      Serial.print(F(" %,  HV: ")); Serial.print(BattDiag.HV,1); Serial.println(F(" V"));
      Serial.println(SPACER);
      Serial.print(F("CV mean : ")); Serial.print(BattDiag.ADCCvolts_mean); Serial.print(F(" mV"));
      Serial.print(F(", ∆ = ")); Serial.print(BattDiag.ADCCvolts_max - BattDiag.ADCCvolts_min); Serial.println(F(" mV"));
      Serial.print(F("CV min  : ")); Serial.print(BattDiag.ADCCvolts_min); Serial.println(F(" mV"));
      Serial.print(F("CV max  : ")); Serial.print(BattDiag.ADCCvolts_max); Serial.println(F(" mV"));
      Serial.println(SPACER);
      Serial.print(F("CAP mean: ")); Serial.print(BattDiag.Cap_mean_As); Serial.print(F(" As/10, ")); Serial.print(BattDiag.Cap_mean_As / 360.0,1); Serial.println(F(" Ah"));
      Serial.print(F("CAP min : ")); Serial.print(BattDiag.Cap_min_As); Serial.print(F(" As/10, ")); Serial.print(BattDiag.Cap_min_As / 360.0,1); Serial.println(F(" Ah"));
      Serial.print(F("CAP max : ")); Serial.print(BattDiag.Cap_max_As); Serial.print(F(" As/10, ")); Serial.print(BattDiag.Cap_max_As / 360.0,1); Serial.println(F(" Ah"));
      Serial.print(F("Last measurement: ")); Serial.print(BattDiag.LastMeas_days); Serial.println(F(" day(s)"));
      Serial.println(SPACER);
      if (BattDiag.HVcontactState == 0x02) {
        Serial.println(F("HV contactor state ON"));
      } else if (BattDiag.HVcontactState == 0x00) {
        Serial.print(F("HV contactor state OFF"));
        Serial.print(F(", for: ")); Serial.print(BattDiag.HVoff_time); Serial.println(F(" s"));
      }
      Serial.print(F("HV contactor cycles left: ")); Serial.println(BattDiag.HVcontactCyclesLeft);
      Serial.print(F("           of max cycles: ")); Serial.println(BattDiag.HVcontactCyclesMax);
      Serial.println(SPACER);
      Serial.print(F("Meas. quality         : ")); Serial.println(BattDiag.Cap_meas_quality,3);
      Serial.print(F("Meas. quality combined: ")); Serial.println(BattDiag.Cap_combined_quality,3);
      Serial.println(SPACER);
 
      if (VERBOSE) {
        Serial.println(F("# ;mV  ;As/10"));
        for(int n = 0; n < CELLCOUNT; n++){
          if (n < 9) Serial.print(F("0"));
          Serial.print(n+1); Serial.print(F(";")); Serial.print(CellVoltage.get(n) - BattDiag.ADCvoltsOffset); Serial.print(F(";")); Serial.println(CellCapacity.get(n));
        }
        Serial.println(SPACER);
        Serial.println(F("Individual Cell Statistics:"));
        Serial.println(SPACER);
        Serial.print(F("CV mean : ")); Serial.print(BattDiag.Cvolts_mean - BattDiag.ADCvoltsOffset,0); Serial.print(F(" mV"));
        Serial.print(F(", ∆ = ")); Serial.print(BattDiag.Cvolts_max - BattDiag.Cvolts_min); Serial.print(F(" mV"));
        Serial.print(F(", σ = ")); Serial.print(BattDiag.Cvolts_stdev); Serial.println(F(" mV"));
        Serial.print(F("CV min  : ")); Serial.print(BattDiag.Cvolts_min - BattDiag.ADCvoltsOffset); Serial.println(F(" mV"));
        Serial.print(F("CV max  : ")); Serial.print(BattDiag.Cvolts_max - BattDiag.ADCvoltsOffset); Serial.println(F(" mV"));
        Serial.println(SPACER);
        Serial.print(F("CAP mean: ")); Serial.print(BattDiag.Ccap_mean_As); Serial.print(F(" As/10, ")); Serial.print(BattDiag.Ccap_mean_As / 360.0,1); Serial.println(F(" Ah"));
        Serial.print(F("CAP min : ")); Serial.print(BattDiag.Ccap_min_As); Serial.print(F(" As/10, ")); Serial.print(BattDiag.Ccap_min_As / 360.0,1); Serial.println(F(" Ah"));
        Serial.print(F("CAP max : ")); Serial.print(BattDiag.Ccap_max_As); Serial.print(F(" As/10, ")); Serial.print(BattDiag.Ccap_max_As / 360.0,1); Serial.println(F(" Ah"));
        Serial.println(SPACER);
      }   
   } else {
      Serial.println(F("---------- Measurement failed !----------"));
      fOK = false;
   } 
   
}