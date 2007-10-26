/*********************************************************************************

 Copyright 2006 MakingThings

 Licensed under the Apache License, 
 Version 2.0 (the "License"); you may not use this file except in compliance 
 with the License. You may obtain a copy of the License at

 http://www.apache.org/licenses/LICENSE-2.0 
 
 Unless required by applicable law or agreed to in writing, software distributed
 under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
 CONDITIONS OF ANY KIND, either express or implied. See the License for
 the specific language governing permissions and limitations under the License.

*********************************************************************************/
#include "config.h"
#include "stdlib.h"
#include <stdio.h>
#include "string.h"
#include "xbee.h"

static bool XBee_GetIOValues( XBeePacket* packet, int *inputs );
void XBeeTask( void* p );

typedef struct
{
  void* XBeeTaskPtr;
} XBee_;

XBee_* XBee;

/** \defgroup XBee
	Communicate with XBee (Zigbee) wireless modules via the Make Controller's serial port.
  
  XBee modules from \b MaxStream are small, cheap ($19 each), wireless \b RF (radio frequency) modules that
  can easily be used with the Make Controller Kit to create projects that require wireless communication.  Check
  http://www.maxstream.net/products/xbee/xbee-oem-rf-module-zigbee.php for more info.
  
  \section Overview
  XBee modules are <b>ZigBee/IEEE 802.15.4</b> compliant and can operate in several modes:
  - Transparent serial port.  Messages in one side magically end up at the other endpoint.  Great for enabling wireless
  communication between 2 Make Controllers.
  - AT command mode.  Send traditional AT commands to configure the module itself (as opposed to having
  the data go straight through via serial mode.
  - Packet (API) mode.  Lower level communication that doesn't have to wait for the module to be in
  AT command mode.  Check the \ref XBeePacketTypes for a description of how these packets are laid out.  
  Be sure to call XBeeConfig_SetPacketApiMode before trying to do anything in this mode.

  The general idea is that you have one XBee module connected directly to your Make Controller Kit, and then
  any number of other XBee modules that can communicate with it in order to get information to and from the
  Make Controller.  Or, several Make Controllers can each have an XBee module connected in order to
  communicate wirelessly among themselves.

  XBee modules also have some digital and analog I/O right on them, which means you can directly connect
  sensors to the XBee modules which will both read the values and send them wirelessly.  Check the XBee doc
  for the appropriate commands to send in order to set this up.

  \section API
  The Make Controller API for working with the XBee modules makes use of the XBee Packet API.  If you simply want to make use
  of the transparent serial port functionality, you don't need to deal with any of this - simply hook up the module to
  your Make Controller and start reading and writing over the serial port.

  The XBee Packet API allows for much more flexible and powerful communication with the modules.  It uses AT commands to 
  configure the XBee module itself, and then a handful of Zigbee specified packet types can be sent and received.  
  See \ref XBeePacketTypes for details on these packet types.  

  \ingroup Controller
  @{
*/

/**	
  Controls the active state of the \b XBee subsystem
  @param state Whether this subsystem is active or not
	@return Zero on success.
*/
int XBee_SetActive( int state )
{
  if( state ) 
  { 
    if( XBee == NULL )
    {
      if( CONTROLLER_OK != Serial_SetActive( 1 ) )
        return CONTROLLER_ERROR_SUBSYSTEM_INACTIVE;

      XBee = MallocWait( sizeof( XBee_ ), 100 );
      #ifdef CROSSWORKS_BUILD
      XBee->XBeeTaskPtr = TaskCreate( XBeeTask, "XBee", 800, 0, 3 );
      #else
      XBee->XBeeTaskPtr = TaskCreate( XBeeTask, "XBee", 1000, 0, 3 );
      #endif // CROSSWORKS_BUILD
    }
  }
  else
  {
    Serial_SetActive( 0 );
    if( XBee->XBeeTaskPtr != NULL )
    {
      TaskDelete( XBee->XBeeTaskPtr );
      XBee->XBeeTaskPtr = NULL;
    }
    if( XBee )
    {
      Free( XBee );
      XBee = NULL;
    }
  }
  return CONTROLLER_OK;
}

/**	
  Read the active state of the \b XBee subsystem
	@return An integer specifying the active state - 1 (active) or 0 (inactive).
*/
int XBee_GetActive( )
{
  return Serial_GetActive( );
}

/**	
  Receive an incoming XBee packet.
  This function will not block, and will return as soon as there's no more incoming data or
  the packet is fully received.  If the packet was not fully received, call the function repeatedly
  until it is.

  Clear out a packet before reading into it with a call to XBee_ResetPacket( )
  @param packet The XBeePacket to receive into.
	@return 1 if a complete packet has been received, 0 if not.
  @see XBeeConfig_SetPacketApiMode( )

  \par Example
  \code
  // we're inside a task here...
  XBeePacket myPacket;
  XBee_ResetPacket( &myPacket );
  while( 1 )
  {
    if( XBee_GetPacket( &myPacket ) )
    {
      // process the new packet
      XBee_ResetPacket( &myPacket ); // then clear it out before reading again
    }
    Sleep( 10 );
  }
  \endcode
*/
int XBee_GetPacket( XBeePacket* packet )
{
  if( CONTROLLER_OK != XBee_SetActive( 1 ) )
    return CONTROLLER_ERROR_SUBSYSTEM_INACTIVE;
  
  while( Serial_GetReadable( ) )
  {
    int newChar = Serial_GetChar( );
    if( newChar == -1 )
      break;

    if( newChar == XBEE_PACKET_STARTBYTE && packet->rxState != XBEE_PACKET_RX_START )
      XBee_ResetPacket( packet ); // in case we get into a weird state

    switch( packet->rxState )
    {
      case XBEE_PACKET_RX_START:
        if( newChar == XBEE_PACKET_STARTBYTE )
          packet->rxState = XBEE_PACKET_RX_LENGTH_1;
        break;
      case XBEE_PACKET_RX_LENGTH_1:
        packet->length = newChar;
        packet->length <<= 8;
        packet->rxState = XBEE_PACKET_RX_LENGTH_2;
        break;
      case XBEE_PACKET_RX_LENGTH_2:
        packet->length += newChar;
        if( packet->length > XBEE_MAX_PACKET_SIZE ) // in case we somehow get some garbage
          packet->length = XBEE_MAX_PACKET_SIZE;
        packet->rxState = XBEE_PACKET_RX_PAYLOAD;
        break;
      case XBEE_PACKET_RX_PAYLOAD:
        *packet->dataPtr++ = newChar;
        if( ++packet->index >= packet->length )
          packet->rxState = XBEE_PACKET_RX_CRC;
        packet->crc += newChar;
        break;
      case XBEE_PACKET_RX_CRC:
        packet->crc += newChar;
        packet->rxState = XBEE_PACKET_RX_START;
        if( packet->crc == 0xFF )
          return 1;
        else
        {
          XBee_ResetPacket( packet );
          return 0;
        }
    }
  }
  return 0;
}

/**	
  Send an XBee packet.

  Check the possible \ref XBeePacketTypes that can be sent, and populate the packet structures
  appropriately before sending them.
  @param packet The XBeePacket to send.
  @param datalength The length of the actual data being sent (not including headers, options, etc.)
	@return Zero on success.
  @see XBeeConfig_SetPacketApiMode( )

  \par Example
  \code
  XBeePacket myPacket;
  packet->apiId = XBEE_TX16; // we're going to send a packet with a 16-bit address
  packet->tx16.frameID = 0x00;
  packet->tx16.destination[0] = 0xFF; // 0xFFFF is the broadcast address
  packet->tx16.destination[1] = 0xFF;
  packet->tx16.options = 0x00; // no options
  packet->tx16.data[0] = 'A'; // now pack in the data
  packet->tx16.data[1] = 'B';
  packet->tx16.data[2] = 'C';
  // finally, send the packet indicating that we're sending 3 bytes of data - "ABC"
  XBee_SendPacket( &myPacket, 3 ); 
  \endcode
*/
int XBee_SendPacket( XBeePacket* packet, int datalength )
{
  if( CONTROLLER_OK != XBee_SetActive( 1 ) )
    return CONTROLLER_ERROR_SUBSYSTEM_INACTIVE;
  
  Serial_SetChar( XBEE_PACKET_STARTBYTE );
  int size = datalength;
  switch( packet->apiId )
  {
    case XBEE_RX64: //account for apiId, 8 bytes source address, signal strength, and options
    case XBEE_TX64: //account for apiId, frameId, 8 bytes destination, and options
      size += 11;
      break;
    case XBEE_RX16: //account for apiId, 2 bytes source address, signal strength, and options
    case XBEE_TX16: //account for apiId, frameId, 2 bytes destination, and options
    case XBEE_ATCOMMANDRESPONSE: // account for apiId, frameID, 2 bytes AT cmd, 1 byte status
      size += 5; 
      break;
    case XBEE_TXSTATUS:
      size = 3; // explicitly set this, since there's no data afterwards
      break;
    case XBEE_ATCOMMAND: // account for apiId, frameID, 2 bytes AT command
    case XBEE_ATCOMMANDQ: // same
      size += 4;
      break;
    default:
      size = 0;
      break;
  }

  Serial_SetChar( (size >> 8) & 0xFF ); // send the most significant bit
  Serial_SetChar( size & 0xFF ); // then the LSB
  packet->crc = 0; // just in case it hasn't been initialized.
  uint8* p = (uint8*)packet;
  while( size-- )
  {
    Serial_SetChar( *p );
    packet->crc += *p++;
  }
  Serial_SetChar( 0xFF - packet->crc );
  return CONTROLLER_OK;
}

/**	
  Initialize a packet before reading into it.
  @param packet The XBeePacket to initialize.
  @see XBee_GetPacket( )
*/
void XBee_ResetPacket( XBeePacket* packet )
{
  packet->dataPtr = (uint8*)packet;
  packet->crc = 0;
  packet->rxState = XBEE_PACKET_RX_START;
  packet->length = 0;
  packet->index = 0;
  packet->apiId = 0;
}

/**	
  Set a module into AT command mode.  
  Because XBee modules need to wait 1 second after sending the command sequence before they're 
  ready to receive any AT commands, this function will block for about a second.
*/
void XBeeConfig_SetPacketApiMode( )
{
  if( CONTROLLER_OK != XBee_SetActive( 1 ) )
    return;
  
  char buf[50];
  snprintf( buf, 50, "+++" ); // enter command mode
  Serial_Write( (uchar*)buf, strlen(buf), 0 );
  Sleep( 1025 ); // have to wait one second after +++ to actually get set to receive in AT mode
  
  snprintf( buf, 50, "ATAP %x,CN\r", 1 ); // turn API mode on, and leave command mode
  Serial_Write( (uchar*)buf, strlen(buf), 0 );
}

/**	
  A convenience function for creating an AT command packet.
  Because XBee modules need to wait 1 second after sending the command sequence before they're 
  ready to receive any AT commands, this function will block for about a second.
  @param packet The XBeePacket to create.
  @param frameID The frame ID for this packet that subsequent response/status messages can refer to.
  @param cmd The 2-character AT command.
  @param params A pointer to the buffer containing the data to be sent.
  @param datalength The number of bytes to send from the params buffer.
	@return An integer specifying the active state - 1 (active) or 0 (inactive).
  
  \par Example
  \code
  XBeePacket txPacket;
  uint8 params[5];
  params[0] = 0x14; // only 1 byte of data in this case
  XBee_CreateATCommandPacket( &txPacket, 0, "IR", params, 1 );
  XBee_SendPacket( &txPacket, 1 );
  \endcode
*/
void XBee_CreateATCommandPacket( XBeePacket* packet, uint8 frameID, char* cmd, uint8* params, uint8 datalength )
{
  packet->apiId = XBEE_ATCOMMAND;
  packet->atCommand.frameID = frameID;
  uint8* p = packet->atCommand.command;
  *p++ = *cmd++;
  *p++ = *cmd++;
  p = packet->atCommand.parameters;
  while( datalength-- )
    *p++ = *params++;
}

/**	
  Configure the IO settings on an XBee module.
  IO pins can have one of 5 values:
  - XBEE_IO_DISABLED
  - XBEE_IO_ANALOGIN - Analog input (10-bit)
  - XBEE_IO_DIGITALIN - Digital input
  - XBEE_IO_DIGOUT_HIGH - Digital out high
  - XBEE_IO_DIGOUT_LOW - Digital out low

  Only channels 0-5 can be analog inputs - channels 6-8 can only operate as digital ins or outs.
  @param ioconfig An array of 9 int values specifying the behavior of that pin.
	@return 1 if IO values were successfully retrieved, otherwise zero.
  
  \par Example
  \code
  int ioChannels[9];
  ioChannels[0] = XBEE_IO_ANALOGIN;
  ioChannels[1] = XBEE_IO_ANALOGIN;
  ioChannels[2] = XBEE_IO_ANALOGIN;
  ioChannels[3] = XBEE_IO_DIGITALIN;
  ioChannels[4] = XBEE_IO_DIGITALIN;
  ioChannels[5] = XBEE_IO_DISABLED;
  ioChannels[6] = XBEE_IO_DISABLED;
  ioChannels[7] = XBEE_IO_DISABLED;
  ioChannels[8] = XBEE_IO_DISABLED;

  XBeeConfig_SetIOs( ioChannels );
  \endcode
*/
void XBeeConfig_SetIOs( int ioconfig[] )
{
  XBeePacket packet;
  XBee_ResetPacket( &packet );
  uint8 params[1];
  char cmd[2];
  int i;
  for( i=0; i < 9; i++ )
  {
    sprintf( (char*)params, "%x", ioconfig[i] );
    sprintf( cmd, "D%d", i );
    XBee_CreateATCommandPacket( &packet, 0, cmd, params, 1 );
    XBee_SendPacket( &packet, 1 );
    XBee_ResetPacket( &packet );
  }
}

/**	
  Create a packet to be transmitted with a 16-bit address.

  @param xbp The XBeePacket to create.
  @param frameID The frame ID for this packet that subsequent response/status messages can refer to.
  @param destination The destination address for this packet.
  @param options The XBee options for this packet (0 if none).
  @param data A pointer to the data to be sent in this packet.
  @param datalength The number of bytes of data to be sent.
	@return True on success, false on failure.
  
  \par Example
  \code
  XBeePacket txPacket;
  uint8 data[] = "ABC";
  XBee_CreateTX16Packet( &txPacket, 0, 0, 0, data, 3 );
  XBee_SendPacket( &txPacket, 3 );
  \endcode
*/
bool XBee_CreateTX16Packet( XBeePacket* xbp, uint8 frameID, uint16 destination, uint8 options, uint8* data, uint8 datalength )
{
  xbp->apiId = XBEE_TX16;
  xbp->tx16.frameID = frameID;
  xbp->tx16.destination[0] = destination >> 8;
  xbp->tx16.destination[1] = destination & 0xFF;
  xbp->tx16.options = options;
  xbp->length = datalength + 5;
  uint8* p = xbp->tx16.data;
  while( datalength-- )
    *p++ = *data++;
  return true;
}

/**	
  Create a packet to be transmitted with a 64-bit address.

  @param xbp The XBeePacket to create.
  @param frameID The frame ID for this packet that subsequent response/status messages can refer to.
  @param destination The destination address for this packet.
  @param options The XBee options for this packet (0 if none).
  @param data A pointer to the data to be sent in this packet.
  @param datalength The number of bytes of data to be sent.
	@return True on success, false on failure.
  
  \par Example
  \code
  XBeePacket txPacket;
  uint8 data[] = "ABCDE";
  XBee_CreateTX16Packet( &txPacket, 0, 0, 0, data, 5 );
  XBee_SendPacket( &txPacket, 5 );
  \endcode
*/
bool XBee_CreateTX64Packet( XBeePacket* xbp, uint8 frameID, uint64 destination, uint8 options, uint8* data, uint8 datalength )
{
  uint8* p;
  int i;
  xbp->apiId = XBEE_TX64;
  xbp->tx64.frameID = frameID;
  for( i = 0; i < 8; i++ )
    xbp->tx64.destination[i] = (destination >> 8*i) & (0xFF * i); // ????????
  xbp->tx64.options = options;
  xbp->length = datalength + 5;
  p = xbp->tx64.data;
  while( datalength-- )
    *p++ = *data++;
  return true;
}

/**	
  Unpack the info from an incoming packet with a 16-bit address.
  Pass \b NULL into any of the parameters you don't care about.
  @param xbp The XBeePacket to read from.
  @param srcAddress The 16-bit address of this packet.
  @param sigstrength The signal strength of this packet.
  @param options The XBee options for this packet.
  @param data A pointer that will be set to the data of this packet.
  @param datalength The length of data in this packet.
	@return True on success, false on failure.
  
  \par Example
  \code
  XBeePacket rxPacket;
  if( XBee_GetPacket( &rxPacket ) )
  {
    uint16 src;
    uint8 sigstrength;
    uint8* data;
    uint8 datalength;
    if( XBee_ReadRX16Packet( &rxPacket, &src, &sigstrength, NULL, &data, &datalength ) )
    {
      // then process the new packet here
      XBee_ResetPacket( &rxPacket ); // and clear it out before reading again
    }
  }
  \endcode
*/
bool XBee_ReadRX16Packet( XBeePacket* xbp, uint16* srcAddress, uint8* sigstrength, uint8* options, uint8** data, uint8* datalength )
{
  if( xbp->apiId != XBEE_RX16 )
    return false;

  if( srcAddress )
  {
    *srcAddress = xbp->rx16.source[0];
    *srcAddress <<= 8;
    *srcAddress += xbp->rx16.source[1];
  }
  if( sigstrength )
    *sigstrength = xbp->rx16.rssi;
  if( options )
    *options = xbp->rx16.options;
  if( data )
    *data = xbp->rx16.data;
  if( datalength )
    *datalength = xbp->length - 5;
  return true;
}

/**	
  Unpack the info from an incoming packet with a 64-bit address.
  Pass \b NULL into any of the parameters you don't care about.
  @param xbp The XBeePacket to read from.
  @param srcAddress The 64-bit address of this packet.
  @param sigstrength The signal strength of this packet.
  @param options The XBee options for this packet.
  @param data A pointer that will be set to the data of this packet.
  @param datalength The length of data in this packet.
	@return True on success, false on failure.
  
  \par Example
  \code
  XBeePacket rxPacket;
  if( XBee_GetPacket( &rxPacket ) )
  {
    uint64 src;
    uint8 sigstrength;
    uint8* data;
    uint8 datalength;
    if( XBee_ReadRX64Packet( &rxPacket, &src, &sigstrength, NULL, &data, &datalength ) )
    {
      // then process the new packet here
      XBee_ResetPacket( &rxPacket ); // and clear it out before reading again
    }
  }
  \endcode
*/
bool XBee_ReadRX64Packet( XBeePacket* xbp, uint64* srcAddress, uint8* sigstrength, uint8* options, uint8** data, uint8* datalength )
{
  if( xbp->apiId != XBEE_RX64 )
    return false;

  int i;
  if( srcAddress )
  {
    for( i = 0; i < 8; i++ )
    {
      *srcAddress <<= i*8;
      *srcAddress += xbp->rx64.source[i];
    }
  }
  if( sigstrength )
    *sigstrength = xbp->rx64.rssi;
  if( options )
    *options = xbp->rx64.options;
  if( data )
    *data = xbp->rx64.data;
  if( datalength )
    *datalength = xbp->length - 11;
  return true;
}

/**	
  Unpack the info from an incoming IO packet with a 16-bit address.
  When an XBee module has been given a sample rate, it will sample its IO pins according to their current configuration
  and send an IO packet with the sample data.  This function will extract the sample info into an array of ints for you.
  There are 9 IO pins on the XBee modules, so be sure that the array you pass in has room for 9 ints.

  Pass \b NULL into any of the parameters you don't care about.
  @param xbp The XBeePacket to read from.
  @param srcAddress A pointer to a uint16 that will be filled up with the 16-bit address of this packet.
  @param sigstrength A pointer to a uint8 that will be filled up with the signal strength of this packet.
  @param options A pointer to a uint8 that will be filled up with the XBee options for this packet.
  @param samples A pointer to an array of ints that will be filled up with the sample values from this packet.
	@return True on success, false on failure.
  @see XBeeConfig_SetSampleRate( ), XBeeConfig_SetIOs( )
  
  \par Example
  \code
  XBeePacket rxPacket;
  if( XBee_GetPacket( &rxPacket ) )
  {
    uint16 src;
    uint8 sigstrength;
    int samples[9];
    if( XBee_ReadIO16Packet( &rxPacket, &src, &sigstrength, NULL, samples ) )
    {
      // then process the new packet here
      XBee_ResetPacket( &rxPacket ); // and clear it out before reading again
    }
  }
  \endcode
*/
bool XBee_ReadIO16Packet( XBeePacket* xbp, uint16* srcAddress, uint8* sigstrength, uint8* options, int* samples )
{
  if( xbp->apiId != XBEE_IO16 )
    return false;
  if( srcAddress )
  {
    *srcAddress = xbp->io16.source[0];
    *srcAddress <<= 8;
    *srcAddress += xbp->io16.source[1];
  }
  if( sigstrength )
    *sigstrength = xbp->io16.rssi;
  if( options )
    *options = xbp->io16.options;
  if( samples )
  {
    if( !XBee_GetIOValues( xbp, samples ) )
      return false;
  }
  return true;
}

/**	
  Unpack the info from an incoming IO packet with a 64-bit address.
  When an XBee module has been given a sample rate, it will sample its IO pins according to their current configuration
  and send an IO packet with the sample data.  This function will extract the sample info into an array of ints for you.
  There are 9 IO pins on the XBee modules, so be sure that the array you pass in has room for 9 ints.

  Pass \b NULL into any of the parameters you don't care about.
  @param xbp The XBeePacket to read from.
  @param srcAddress A pointer to a uint64 that will be filled up with the 16-bit address of this packet.
  @param sigstrength A pointer to a uint8 that will be filled up with the signal strength of this packet.
  @param options A pointer to a uint8 that will be filled up with the XBee options for this packet.
  @param samples A pointer to an array of ints that will be filled up with the sample values from this packet.
	@return True on success, false on failure.
  @see XBeeConfig_SetSampleRate( ), XBeeConfig_SetIOs( )
  
  \par Example
  \code
  XBeePacket rxPacket;
  if( XBee_GetPacket( &rxPacket ) )
  {
    uint64 src;
    uint8 sigstrength;
    int samples[9];
    if( XBee_ReadIO16Packet( &rxPacket, &src, &sigstrength, NULL, samples ) )
    {
      // then process the new packet here
      XBee_ResetPacket( &rxPacket ); // and clear it out before reading again
    }
  }
  \endcode
*/
bool XBee_ReadIO64Packet( XBeePacket* xbp, uint64* srcAddress, uint8* sigstrength, uint8* options, int* samples )
{
  if( xbp->apiId != XBEE_RX64 )
    return false;
  if( srcAddress )
  {
    int i;
    for( i = 0; i < 8; i++ )
    {
      *srcAddress <<= i*8;
      *srcAddress += xbp->io64.source[i];
    }
  }
  if( sigstrength )
    *sigstrength = xbp->io64.rssi;
  if( options )
    *options = xbp->io64.options;
  if( samples )
  {
    if( !XBee_GetIOValues( xbp, samples ) )
      return false;
  }
  return true;
}

/**	
  Unpack the info from an incoming AT Command Response packet.
  In response to a previous AT Command message, the module will send an AT Command Response message.

  Pass \b NULL into any of the parameters you don't care about.
  @param xbp The XBeePacket to read from.
  @param frameID A pointer to a uint64 that will be filled up with the 16-bit address of this packet.
  @param command A pointer to a uint8 that will be filled up with the signal strength of this packet.
  @param status A pointer to a uint8 that will be filled up with the XBee options for this packet.
  @param data A pointer that will be set to the data of this packet.
	@return True on success, false on failure.
  
  \par Example
  \code
  XBeePacket rxPacket;
  if( XBee_GetPacket( &rxPacket ) )
  {
    uint8 frameID;
    char* command;
    uint8 status;
    uint8* data;
    if( XBee_ReadAtResponsePacket( &rxPacket, &frameID, command, &status, &data ) )
    {
      // then process the new packet here
      XBee_ResetPacket( &rxPacket ); // and clear it out before reading again
    }
  }
  \endcode
*/
bool XBee_ReadAtResponsePacket( XBeePacket* xbp, uint8* frameID, char* command, uint8* status, uint8** data )
{
  if( xbp->apiId != XBEE_ATCOMMANDRESPONSE )
    return false;
  if( frameID )
    *frameID = xbp->atResponse.frameID;
  if( command )
    command = (char*)xbp->atResponse.command;
  if( status )
    *status = xbp->atResponse.status;
  if( data )
    *data = xbp->atResponse.value;
  return true;
}

/**	
  Unpack the info from an incoming AT Command Response packet.
  When a TX is completed, the modules esnds a TX Status message.  This indicates whether the packet was transmitted
  successfully or not.

  Pass \b NULL into any of the parameters you don't care about.
  @param xbp The XBeePacket to read from.
  @param frameID A pointer to a uint64 that will be filled up with the 16-bit address of this packet.
  @param command A pointer to a uint8 that will be filled up with the signal strength of this packet.
  @param status A pointer to a uint8 that will be filled up with the XBee options for this packet.
  @param data A pointer that will be set to the data of this packet.
	@return True on success, false on failure.
  
  \par Example
  \code
  XBeePacket rxPacket;
  if( XBee_GetPacket( &rxPacket ) )
  {
    uint8 frameID;
    char* command;
    uint8 status;
    uint8* data;
    if( XBee_ReadAtResponsePacket( &rxPacket, &frameID, command, &status, &data ) )
    {
      // then process the new packet here
      XBee_ResetPacket( &rxPacket ); // and clear it out before reading again
    }
  }
  \endcode
*/
bool XBee_ReadTXStatusPacket( XBeePacket* xbp, uint8* frameID, uint8* status )
{
  if( xbp->apiId != XBEE_TXSTATUS )
    return false;
  if( frameID )
    *frameID = xbp->txStatus.frameID;
  if( status )
    *status = xbp->txStatus.status;
  return true;
}

/**	
  Save the configuration changes you've made on the module to memory.
  When you make configuration changes - setting the module to API mode, or configuring the sample rate, for example - 
  those changes will be lost when the module restarts.  Call this function to save the current state to non-volatile memory.

  As with the other \b XBeeConfig functions, make sure you're in API mode before trying to use this function.
	@return True on success, false on failure.
  @see XBeeConfig_SetPacketApiMode( )
  
  \par Example
  \code
  XBeeConfig_SetPacketApiMode( );
  XBeeConfig_SetSampleRate( 100 );
  XBeeConfig_WriteStateToMemory( );
  \endcode
*/
void XBeeConfig_WriteStateToMemory( void )
{
  XBeePacket xbp;
  XBee_CreateATCommandPacket( &xbp, 0, "WR", NULL, 0 );
  XBee_SendPacket( &xbp, 0 );
}

/**	
  Set this module's address.
  When you make configuration changes - setting the module to API mode, or configuring the sample rate, for example - 
  those changes will be lost when the module restarts.  Call this function to save the current state to non-volatile memory.

  As with the other \b XBeeConfig functions, make sure you're in API mode before trying to use this function.
  @param address An integer specifying the module's address.
	@return True on success, false on failure.
  @see XBeeConfig_SetPacketApiMode( )
  
  \par Example
  \code
  XBeeConfig_SetPacketApiMode( );
  XBeeConfig_SetSampleRate( 100 );
  XBeeConfig_WriteStateToMemory( );
  \endcode
*/
void XBeeConfig_SetAddress( int address )
{
  XBeePacket xbp;
  uint8 params[4];
  params[0] = address & 0xFF;
  params[1] = (address >> 8) & 0xFFFFFF;
  params[2] = (address >> 16) & 0xFFFF;
  params[3] = (address >> 24) & 0xFF;
  XBee_CreateATCommandPacket( &xbp, 0, "IR", params, 4 );
  XBee_SendPacket( &xbp, 4 );
}

/**	
  Set this PAN (Personal Area Network) ID.
  Only modules with matching PAN IDs can communicate with each other.  Unique PAN IDs enable control of which
  RF packets are received by a module.  Default is \b 0x3332.

  As with the other \b XBeeConfig functions, make sure you're in API mode before trying to use this function.
  @param id A uint16 specifying the PAN ID.
	@return True on success, false on failure.
  @see XBeeConfig_SetPacketApiMode( )
  
  \par Example
  \code
  XBeeConfig_SetPacketApiMode( );
  XBeeConfig_SetPanID( 0x1234 );
  \endcode
*/
void XBeeConfig_SetPanID( uint16 id )
{
  XBeePacket xbp;
  uint8 params[2];
  params[0] = (id >> 8) & 0xFF;
  params[1] = id & 0xFF;
  XBee_CreateATCommandPacket( &xbp, 0, "ID", params, 2 );
  XBee_SendPacket( &xbp, 2 );
}

/**	
  Set the module's channel.
  The channel is one of 3 addressing options available to the module - the others are the PAN ID and the
  destination address.  In order for modules to communicate with each other, the modules must share the same
  channel number.  Default is \b 0x0C.

  This value can have the range \b 0x0B - \b 0x1A for XBee modules, and \b 0x0C - \b 0x17 for XBee-Pro modules.

  As with the other \b XBeeConfig functions, make sure you're in API mode before trying to use this function.
  @param channel A uint8 specifying the channel.
	@return True on success, false on failure.
  @see XBeeConfig_SetPacketApiMode( )
  
  \par Example
  \code
  XBeeConfig_SetPacketApiMode( );
  XBeeConfig_SetChannel( 0x0D );
  \endcode
*/
void XBeeConfig_SetChannel( uint8 channel )
{
  XBeePacket xbp;
  uint8 params[1];
  params[0] = channel;
  XBee_CreateATCommandPacket( &xbp, 0, "CH", params, 1 );
  XBee_SendPacket( &xbp, 1 );
}

/**	
  Set the rate at which the module will sample its IO pins.
  When the sample rate is set, the module will sample all its IO pins according to its current IO configuration and send
  a packet with the sample data.  If this is set too low and the IO configuration is sampling too many channels, 
  the RF module won't be able to keep up.  You can also adjust how many samples are gathered before a packet is sent.

  As with the other \b XBeeConfig functions, make sure you're in API mode before trying to use this function.
  @param rate A uint16 specifying the sample rate in milliseconds.
	@return True on success, false on failure.
  @see XBeeConfig_SetIOs( ), XBeeConfig_SetPacketApiMode( )
  
  \par Example
  \code
  XBeeConfig_SetPacketApiMode( );
  XBeeConfig_SetPanID( 0x1234 );
  \endcode
*/
void XBeeConfig_SetSampleRate( uint16 rate )
{
  XBeePacket xbp;
  uint8 params[2];
  params[0] = (rate >> 8) & 0xFF;
  params[1] = rate & 0xFF;
  XBee_CreateATCommandPacket( &xbp, 0, "IR", params, 2 );
  XBee_SendPacket( &xbp, 2 );
}

/** @}
*/

/**	
  Unpack IO values from an incoming packet.
  @param packet The XBeePacket to read from.
  @param inputs An array of at least 9 integers, which will be populated with the values of the 9 input lines on the XBee module.
	@return 1 if IO values were successfully retrieved, otherwise zero.
  
  \par Example
  \code
  XBeePacket rxPacket;
  if( XBee_GetPacket( &rxPacket ) )
  {
    int inputs[9];
    if( XBee_GetIOValues( &rxPacket, inputs ) )
    {
      // process new input values here
    }
  }
  \endcode
*/
static bool XBee_GetIOValues( XBeePacket* packet, int *inputs )
{
  if( packet->apiId == XBEE_IO16 || packet->apiId == XBEE_IO64 )
  {
    int i;
    static bool enabled;
    int digitalins = 0;
    uint8* p;
    int channelIndicators;
    if( packet->apiId == XBEE_IO16 )
    {
      p = packet->io16.data;
      channelIndicators = (packet->io16.channelIndicators[0] << 0x08) | packet->io16.channelIndicators[1];
    }
    else // packet->apiId == XBEE_IO64
    {
      p = packet->io64.data;
      channelIndicators = (packet->io64.channelIndicators[0] << 0x08) | packet->io64.channelIndicators[1];
    }
    
    for( i = 0; i < XBEE_INPUTS; i++ )
    {
      enabled = channelIndicators & 1;
      channelIndicators >>= 1;
      if( i < 9 ) // digital ins
      {
        if( enabled )
        {
          if( !digitalins )
          {
            int dig0 = *p++ << 0x08;
            digitalins = dig0 | *p++;
          }
          inputs[i] = ((digitalins >> i) & 1) * 1023;
        }
        else
          inputs[i] = 0;
      }
      else // analog ins
      {
        if( enabled )
        {
          int ain_msb = *p++ << 0x08;
          inputs[i-9] = ain_msb | *p++;
        }
        else
          inputs[i-9] = 0;
      }
    }
    return true;
  }
  else
    return false;
}


#ifdef OSC
#include "osc.h"

static char* XBeeOsc_Name = "xbee";
static char* XBeeOsc_PropertyNames[] = { "active", 0 }; // must have a trailing 0

int XBeeOsc_PropertySet( int property, char* typedata, int channel );
int XBeeOsc_PropertyGet( int property, int channel );

const char* XBeeOsc_GetName( void )
{
  return XBeeOsc_Name;
}

int XBeeOsc_ReceiveMessage( int channel, char* message, int length )
{
  int status = Osc_GeneralReceiverHelper( channel, message, length, 
                                XBeeOsc_Name,
                                XBeeOsc_PropertySet, XBeeOsc_PropertyGet, 
                                XBeeOsc_PropertyNames );

  if ( status != CONTROLLER_OK )
    return Osc_SendError( channel, XBeeOsc_Name, status );

  return CONTROLLER_OK;
}

int XBeeOsc_PropertySet( int property, char* typedata, int channel )
{
  switch ( property )
  {
    case 0: // active
    {
      int value;
      int count = Osc_ExtractData( typedata, "i", &value );
      if ( count != 1 )
        return Osc_SubsystemError( channel, XBeeOsc_Name, "Incorrect data - need an int" );

      XBee_SetActive( value );
      break;
    }
  }
  return CONTROLLER_OK;
}

int XBeeOsc_PropertyGet( int property, int channel )
{
  int value = 0;
  char address[ OSC_SCRATCH_SIZE ];
  switch ( property )
  {
    case 0: // active
      value = XBee_GetActive( );
      snprintf( address, OSC_SCRATCH_SIZE, "/%s/%s", XBeeOsc_Name, XBeeOsc_PropertyNames[ property ] ); 
      Osc_CreateMessage( channel, address, ",i", value );
      break;
  }
  return CONTROLLER_OK;
}

void XBeeTask( void* p )
{
 (void)p;
  XBeePacket myPacket;
  XBee_ResetPacket( &myPacket );
  XBeeConfig_SetPacketApiMode( );

  int in[9];

  while( 1 )
  {
    if( XBee_GetPacket( &myPacket ) )
    {
      XBee_ReadIO16Packet( &myPacket, NULL, NULL, NULL, in );
      XBee_ResetPacket( &myPacket ); // then clear it out before reading again
    }
    Sleep( 5 );
  }
}

#endif // OSC


