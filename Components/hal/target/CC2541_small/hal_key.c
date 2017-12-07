/******************************************************************************

 @file  hal_key.c

 @brief This file contains the interface to the HAL KEY Service.

 Group: WCS, BTS
 Target Device: CC2540, CC2541

 ******************************************************************************
 
 Copyright (c) 2006-2016, Texas Instruments Incorporated
 All rights reserved.

 IMPORTANT: Your use of this Software is limited to those specific rights
 granted under the terms of a software license agreement between the user
 who downloaded the software, his/her employer (which must be your employer)
 and Texas Instruments Incorporated (the "License"). You may not use this
 Software unless you agree to abide by the terms of the License. The License
 limits your use, and you acknowledge, that the Software may not be modified,
 copied or distributed unless embedded on a Texas Instruments microcontroller
 or used solely and exclusively in conjunction with a Texas Instruments radio
 frequency transceiver, which is integrated into your product. Other than for
 the foregoing purpose, you may not use, reproduce, copy, prepare derivative
 works of, modify, distribute, perform, display or sell this Software and/or
 its documentation for any purpose.

 YOU FURTHER ACKNOWLEDGE AND AGREE THAT THE SOFTWARE AND DOCUMENTATION ARE
 PROVIDED �AS IS� WITHOUT WARRANTY OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 INCLUDING WITHOUT LIMITATION, ANY WARRANTY OF MERCHANTABILITY, TITLE,
 NON-INFRINGEMENT AND FITNESS FOR A PARTICULAR PURPOSE. IN NO EVENT SHALL
 TEXAS INSTRUMENTS OR ITS LICENSORS BE LIABLE OR OBLIGATED UNDER CONTRACT,
 NEGLIGENCE, STRICT LIABILITY, CONTRIBUTION, BREACH OF WARRANTY, OR OTHER
 LEGAL EQUITABLE THEORY ANY DIRECT OR INDIRECT DAMAGES OR EXPENSES
 INCLUDING BUT NOT LIMITED TO ANY INCIDENTAL, SPECIAL, INDIRECT, PUNITIVE
 OR CONSEQUENTIAL DAMAGES, LOST PROFITS OR LOST DATA, COST OF PROCUREMENT
 OF SUBSTITUTE GOODS, TECHNOLOGY, SERVICES, OR ANY CLAIMS BY THIRD PARTIES
 (INCLUDING BUT NOT LIMITED TO ANY DEFENSE THEREOF), OR OTHER SIMILAR COSTS.

 Should you have any questions regarding your right to use this Software,
 contact Texas Instruments Incorporated at www.TI.com.

 ******************************************************************************
 Release Name: ble_sdk_1.4.2.2
 Release Date: 2016-06-09 06:57:09
 *****************************************************************************/
/*********************************************************************
 NOTE: If polling is used, the hal_driver task schedules the KeyRead()
       to occur every 100ms.  This should be long enough to naturally
       debounce the keys.  The KeyRead() function remembers the key
       state of the previous poll and will only return a non-zero
       value if the key state changes.

 NOTE: If interrupts are used, the KeyRead() function is scheduled
       25ms after the interrupt occurs by the ISR.  This delay is used
       for key debouncing.  The ISR disables any further Key interrupt
       until KeyRead() is executed.  KeyRead() will re-enable Key
       interrupts after executing.  Unlike polling, when interrupts
       are enabled, the previous key state is not remembered.  This
       means that KeyRead() will return the current state of the keys
       (not a change in state of the keys).

 NOTE: If interrupts are used, the KeyRead() fucntion is scheduled by
       the ISR.  Therefore, the joystick movements will only be detected
       during a pushbutton interrupt caused by S1 or the center joystick
       pushbutton.

 NOTE: When a switch like S1 is pushed, the S1 signal goes from a normally
       high state to a low state.  This transition is typically clean.  The
       duration of the low state is around 200ms.  When the signal returns
       to the high state, there is a high likelihood of signal bounce, which
       causes a unwanted interrupts.  Normally, we would set the interrupt
       edge to falling edge to generate an interrupt when S1 is pushed, but
       because of the signal bounce, it is better to set the edge to rising
       edge to generate an interrupt when S1 is released.  The debounce logic
       can then filter out the signal bounce.  The result is that we typically
       get only 1 interrupt per button push.  This mechanism is not totally
       foolproof because occasionally, signal bound occurs during the falling
       edge as well.  A similar mechanism is used to handle the joystick
       pushbutton on the DB.  For the EB, we do not have independent control
       of the interrupt edge for the S1 and center joystick pushbutton.  As
       a result, only one or the other pushbuttons work reasonably well with
       interrupts.  The default is the make the S1 switch on the EB work more
       reliably.

*********************************************************************/

/**************************************************************************************************
 *                                            INCLUDES
 **************************************************************************************************/
#include "hal_mcu.h"
#include "hal_defs.h"
#include "hal_types.h"
#include "hal_drivers.h"
#include "hal_adc.h"
#include "hal_key.h"
#include "osal.h"
#include "..\..\util\SBL\app\sbl_app.h"
#include "hal_uart.h"
#if (defined HAL_KEY) && (HAL_KEY == TRUE)

/**************************************************************************************************
 *                                            CONSTANTS
 **************************************************************************************************/
#define HAL_KEY_RISING_EDGE   0
#define HAL_KEY_FALLING_EDGE  1

#define HAL_KEY_DEBOUNCE_VALUE  25

/* CPU port interrupt */
#define HAL_KEY_CPU_PORT_0_IF P0IF
#define HAL_KEY_CPU_PORT_1_IF P1IF
//#define HAL_KEY_CPU_PORT_2_IF P2IF

//#if defined ( CC2540_MINIDK )
/* BOOT is at P1.0 */
#define HAL_KEY_BOOT_PORT   P0
#define HAL_KEY_BOOT_BIT    BV(2)
#define HAL_KEY_BOOT_SEL    P0SEL
#define HAL_KEY_BOOT_DIR    P0DIR
#define HAL_KEY_BOOT_IEN      IEN1  /* CPU interrupt mask register */
#define HAL_KEY_BOOT_IENBIT   BV(5) /* Mask bit for all of Port_1 */
#define HAL_KEY_BOOT_ICTL     P0IEN /* Port Interrupt Control register */
#define HAL_KEY_BOOT_ICTLBIT  BV(2) /* P1IEN - P0.0 enable/disable bit */
#define HAL_KEY_BOOT_PXIFG    P0IFG /* Interrupt flag at source */
#define HAL_KEY_BOOT_EDGEBIT  HAL_KEY_FALLING_EDGE

///* SW_1 is at P0.0 */
#define HAL_KEY_SW_1_PORT   P0
#define HAL_KEY_SW_1_BIT    BV(0)
#define HAL_KEY_SW_1_SEL    P0SEL
#define HAL_KEY_SW_1_DIR    P0DIR
/* SW_1 interrupts */
#define HAL_KEY_SW_1_IEN      IEN1  /* CPU interrupt mask register */
#define HAL_KEY_SW_1_IENBIT   BV(5) /* Port 0 interrupt enable */
#define HAL_KEY_SW_1_ICTL     P0IEN /* Port Interrupt Control register */
#define HAL_KEY_SW_1_ICTLBIT  BV(0) /* P0IEN - P0.1 enable/disable bit */
#define HAL_KEY_SW_1_PXIFG    P0IFG /* Interrupt flag at source */
//
/* SW_2 is at P0.1 */
#define HAL_KEY_SW_2_PORT   P0
#define HAL_KEY_SW_2_BIT    BV(1)
#define HAL_KEY_SW_2_SEL    P0SEL
#define HAL_KEY_SW_2_DIR    P0DIR

/* edge interrupt */
#define HAL_KEY_SW_2_EDGEBIT  BV(0)
#define HAL_KEY_SW_2_EDGE     HAL_KEY_FALLING_EDGE

/* SW_2 interrupts */
#define HAL_KEY_SW_2_IEN      IEN1  /* CPU interrupt mask register */
#define HAL_KEY_SW_2_IENBIT   BV(5) /* Port 0 interrupt enable */
#define HAL_KEY_SW_2_ICTL     P0IEN /* Port Interrupt Control register */
#define HAL_KEY_SW_2_ICTLBIT    BV(1) /* P0IEN - P0.1 enable/disable bit */
#define HAL_KEY_SW_2_PXIFG  P0IFG
/**************************************************************************************************
 *                                            TYPEDEFS
 **************************************************************************************************/


/**************************************************************************************************
 *                                        GLOBAL VARIABLES
 **************************************************************************************************/
static uint8 halKeySavedKeys;     /* used to store previous key state in polling mode */
static halKeyCBack_t pHalKeyProcessFunction;
static uint8 HalKeyConfigured;
bool Hal_KeyIntEnable;            /* interrupt enable/disable flag */
extern uint8 uartTxBuffer[];
extern uint8 simpleBLEPeripheral_TaskID;
/**************************************************************************************************
 *                                        FUNCTIONS - Local
 **************************************************************************************************/
void halProcessKeyInterrupt(void);




/**************************************************************************************************
 *                                        FUNCTIONS - API
 **************************************************************************************************/
extern void print_event(uint16);

/**************************************************************************************************
 * @fn      HalKeyInit
 *
 * @brief   Initilize Key Service
 *
 * @param   none
 *
 * @return  None
 **************************************************************************************************/
void HalKeyInit( void )
{
  halKeySavedKeys = 0;  // Initialize previous key to 0.


  HAL_KEY_BOOT_SEL &= ~(HAL_KEY_BOOT_BIT);    /* Set pin function to GPIO */
  HAL_KEY_BOOT_DIR &= ~(HAL_KEY_BOOT_BIT);    /* Set pin direction to Input */
  HAL_KEY_SW_1_SEL &= ~(HAL_KEY_SW_1_BIT);    /* Set pin function to GPIO */
  HAL_KEY_SW_1_DIR &= ~(HAL_KEY_SW_1_BIT);    /* Set pin direction to Input */
  HAL_KEY_SW_2_SEL &= ~(HAL_KEY_SW_2_BIT);    /* Set pin function to GPIO */
  HAL_KEY_SW_2_DIR &= ~(HAL_KEY_SW_2_BIT);    /* Set pin direction to Input */

    P0INP |= (LED1_BV|LED2_BV|LED3_BV); //pull UP /down input mode but leds out 3 state
    P2INP =0; //pull UP ALL PORT0 1 2 Selects function for all Port 0 pins configured as pullup-orpulldown inputs.
// 
//  P2INP |= PUSH2_BV;  /* Configure GPIO tri-state. */
//#endif

  /* Initialize callback function */
  pHalKeyProcessFunction  = NULL;

  /* Start with key is not configured */
  HalKeyConfigured = FALSE;

//#if defined ( CC2540_MINIDK )
  /* Rising/Falling edge configuratinn */
  PICTL |= HAL_KEY_BOOT_EDGEBIT;   /* falling edge on input gives interrupt. P1.0 */
  PICTL |= HAL_KEY_SW_2_EDGEBIT; /*falling P0.1 P0.2*/
  
  HAL_KEY_BOOT_IEN |= ( HAL_KEY_BOOT_IENBIT);   /* enable CPU interrupt */
  HAL_KEY_SW_1_IEN |= ( HAL_KEY_SW_1_IENBIT | HAL_KEY_SW_2_IENBIT );/* enable CPU interrupt */
//#endif

  osal_set_event(simpleBLEPeripheral_TaskID, BOOT_EVT);
}


/**************************************************************************************************
 * @fn      HalKeyConfig
 *
 * @brief   Configure the Key serivce
 *
 * @param   interruptEnable - TRUE/FALSE, enable/disable interrupt
 *          cback - pointer to the CallBack function
 *
 * @return  None
 **************************************************************************************************/
void HalKeyConfig (bool interruptEnable, halKeyCBack_t cback)
{
  /* Enable/Disable Interrupt or */
  Hal_KeyIntEnable = interruptEnable;

  /* Register the callback fucntion */
  pHalKeyProcessFunction = cback;

  /* Determine if interrupt is enable or not */
  if (Hal_KeyIntEnable)
  {
//#if defined ( CC2540_MINIDK )
    HAL_KEY_BOOT_ICTL |= HAL_KEY_BOOT_ICTLBIT; /* enable interrupt generation at port */    
    HAL_KEY_BOOT_PXIFG = ~(HAL_KEY_BOOT_BIT);  /* Clear any pending interrupt */
    HAL_KEY_SW_1_ICTL |= HAL_KEY_SW_1_ICTLBIT; /* enable interrupt generation at port */
    HAL_KEY_SW_1_PXIFG = ~(HAL_KEY_SW_1_BIT);  /* Clear any pending interrupt */
    HAL_KEY_SW_2_ICTL |= HAL_KEY_SW_2_ICTLBIT; /* enable interrupt generation at port */
    HAL_KEY_SW_2_PXIFG = ~(HAL_KEY_SW_2_BIT);  /* Clear any pending interrupt */

//#else
//    /* Rising/Falling edge configuratinn */
//    PICTL &= ~(HAL_KEY_SW_6_EDGEBIT);    /* Clear the edge bit */
//    /* For falling edge, the bit must be set. */
//  #if (HAL_KEY_SW_6_EDGE == HAL_KEY_FALLING_EDGE)
//    PICTL |= HAL_KEY_SW_6_EDGEBIT;
//  #endif
//

    /* Interrupt configuration:
     * - Enable interrupt generation at the port
     * - Enable CPU interrupt
     * - Clear any pending interrupt
     */
//    HAL_KEY_SW_6_ICTL |= HAL_KEY_SW_6_ICTLBIT;
//    HAL_KEY_SW_6_IEN |= HAL_KEY_SW_6_IENBIT;
//    HAL_KEY_SW_6_PXIFG = ~(HAL_KEY_SW_6_BIT);

    /* Rising/Falling edge configuratinn */
//    HAL_KEY_JOY_MOVE_ICTL &= ~(HAL_KEY_JOY_MOVE_EDGEBIT);    /* Clear the edge bit */
    /* For falling edge, the bit must be set. */
//  #if (HAL_KEY_JOY_MOVE_EDGE == HAL_KEY_FALLING_EDGE)
//    HAL_KEY_JOY_MOVE_ICTL |= HAL_KEY_JOY_MOVE_EDGEBIT;
//  #endif


    /* Interrupt configuration:
     * - Enable interrupt generation at the port
     * - Enable CPU interrupt
     * - Clear any pending interrupt
     */
//    HAL_KEY_JOY_MOVE_ICTL |= HAL_KEY_JOY_MOVE_ICTLBIT;
//    HAL_KEY_JOY_MOVE_IEN |= HAL_KEY_JOY_MOVE_IENBIT;
//    HAL_KEY_JOY_MOVE_PXIFG = ~(HAL_KEY_JOY_MOVE_BIT);
//#endif // !CC2540_MINIDK

    /* Do this only after the hal_key is configured - to work with sleep stuff */
    if (HalKeyConfigured == TRUE)
    {
      osal_stop_timerEx(Hal_TaskID, HAL_KEY_EVENT);  /* Cancel polling if active */
    }
  }
  else    /* Interrupts NOT enabled */
  {
//#if defined ( CC2540_MINIDK )
    HAL_KEY_BOOT_ICTL &= ~(HAL_KEY_BOOT_ICTLBIT); /* don't generate interrupt */
    HAL_KEY_SW_1_ICTL &= ~(HAL_KEY_SW_1_ICTLBIT); /* don't generate interrupt */
    HAL_KEY_SW_2_ICTL &= ~(HAL_KEY_SW_2_ICTLBIT); /* don't generate interrupt */
//#else
//    HAL_KEY_SW_6_ICTL &= ~(HAL_KEY_SW_6_ICTLBIT); /* don't generate interrupt */
//    HAL_KEY_SW_6_IEN &= ~(HAL_KEY_SW_6_IENBIT);   /* Clear interrupt enable bit */
//#endif  // !CC2540_MINIDK

    osal_set_event(Hal_TaskID, HAL_KEY_EVENT);
  }
  
  
#ifdef DEBUG_GPIO
      // TEMP
      P1DIR |= 1;   //p1_0 - output
#endif // DEBUG_GPIO

  /* Key now is configured */
  HalKeyConfigured = TRUE;
}


/**************************************************************************************************
 * @fn      HalKeyRead
 *
 * @brief   Read the current value of a key
 *
 * @param   None
 *
 * @return  keys - current keys status
 **************************************************************************************************/
uint8 HalKeyRead ( void )
{
  uint8 keys = 0;

//#if defined (CC2540_MINIDK)
  if (!(HAL_KEY_BOOT_PORT & HAL_KEY_BOOT_BIT))    /* Key is active low */
  {
    keys |= HAL_KEY_BOOT;
  }
  if (!(HAL_KEY_SW_1_PORT & HAL_KEY_SW_1_BIT))    /* Key is active low */
  {
    keys |= HAL_KEY_SW_1;
    
  }
  if (!(HAL_KEY_SW_2_PORT & HAL_KEY_SW_2_BIT))    /* Key is active low */
  {
    keys |= HAL_KEY_SW_2;
  }
//#else
//#ifdef HAL_BOARD_CC2530EB_REV17
//  if ( (HAL_KEY_SW_6_PORT & HAL_KEY_SW_6_BIT))    /* Key is active high */
//#elif defined (HAL_BOARD_CC2530EB_REV13)
//  if (!(HAL_KEY_SW_6_PORT & HAL_KEY_SW_6_BIT))    /* Key is active low */
//#endif
//  {
//    keys |= HAL_KEY_SW_6;
//  }
//
//  if ((HAL_KEY_JOY_MOVE_PORT & HAL_KEY_JOY_MOVE_BIT))  /* Key is active low */
//  {
//    keys |= halGetJoyKeyInput();
//  }
//#endif
  return keys;
}


/**************************************************************************************************
 * @fn      HalKeyPoll
 *
 * @brief   Called by hal_driver to poll the keys
 *
 * @param   None
 *
 * @return  None
 **************************************************************************************************/
void HalKeyPoll (void)
{
  uint8 keys = 0;
  uint8 notify = 0;
//#if defined (CC2540_MINIDK)
  if (!(HAL_KEY_BOOT_PORT & HAL_KEY_BOOT_BIT))    /* Key is active HIGH */
  {
    keys |= HAL_KEY_BOOT;
  }
  if (!(HAL_KEY_SW_1_PORT & HAL_KEY_SW_1_BIT))    /* Key is active low */
  {
    keys |= HAL_KEY_SW_1;
    
//    uartTxBuffer[0]=  '\n';
//    uartTxBuffer[1]=  '\r';
//    uartTxBuffer[2]=  'K';
//    uartTxBuffer[3]=  'e';
//    uartTxBuffer[4]=  'y';
//    uartTxBuffer[5]=  ' ';
//    uartTxBuffer[6]=  '1';
//        
//    HalUARTWrite(HAL_UART_PORT_1,uartTxBuffer,7);
  }

  if (!(HAL_KEY_SW_2_PORT & HAL_KEY_SW_2_BIT))    /* Key is active low */
  {
    keys |= HAL_KEY_SW_2;
        
//    uartTxBuffer[0]=  '\n';
//    uartTxBuffer[1]=  '\r';
//    uartTxBuffer[2]=  'K';
//    uartTxBuffer[3]=  'e';
//    uartTxBuffer[4]=  'y';
//    uartTxBuffer[5]=  ' ';
//    uartTxBuffer[6]=  '2';
//        
//    HalUARTWrite(HAL_UART_PORT_1,uartTxBuffer,7);
  }
//
//  if ((HAL_KEY_JOY_MOVE_PORT & HAL_KEY_JOY_MOVE_BIT))  /* Key is active HIGH */
//  {
//    keys = halGetJoyKeyInput();
//  }
//#endif

  /* If interrupts are not enabled, previous key status and current key status
   * are compared to find out if a key has changed status.
   */
  if (!Hal_KeyIntEnable)
  {
    if (keys == halKeySavedKeys)
    {
      /* Exit - since no keys have changed */
      return;
    }
    else
    {
      notify = 1;
    }
  }
  else
  {
    /* Key interrupt handled here */
    if ((keys & HAL_KEY_BOOT))
    {
        osal_set_event(simpleBLEPeripheral_TaskID, BOOT_EVT);
      
      
        uartTxBuffer[0]=  '\n';
        uartTxBuffer[1]=  '\r';
        uartTxBuffer[2]=  'B';
        uartTxBuffer[3]=  'o';
        uartTxBuffer[4]=  'o';
        uartTxBuffer[5]=  't';
        uartTxBuffer[6]=  ' ';
        uartTxBuffer[7]=  'r';
        uartTxBuffer[8]=  'u';
        uartTxBuffer[9]=  'n';
        
        HalUARTWrite(HAL_UART_PORT_1,uartTxBuffer,10);
        
        osal_start_timerEx( simpleBLEPeripheral_TaskID, BOOT_EVT, 100);
    }
    notify = 1;
  }

  /* Store the current keys for comparation next time */
  halKeySavedKeys = keys;

  /* Invoke Callback if new keys were depressed */
  if (notify && (pHalKeyProcessFunction))
  {
    (pHalKeyProcessFunction) (keys, HAL_KEY_STATE_NORMAL);
  }
}



/**************************************************************************************************
 * @fn      halProcessKeyInterrupt
 *
 * @brief   Checks to see if it's a valid key interrupt, saves interrupt driven key states for
 *          processing by HalKeyRead(), and debounces keys by scheduling HalKeyRead() 25ms later.
 *
 * @param
 *
 * @return
 **************************************************************************************************/
void halProcessKeyInterrupt (void)
{
  bool valid=FALSE;
  
//print_event(0xFF);

  if( HAL_KEY_BOOT_PXIFG & HAL_KEY_BOOT_BIT) /* Interrupt Flag has been set by BOOT */
      {
        HAL_KEY_BOOT_PXIFG = ~(HAL_KEY_BOOT_BIT); /* Clear Interrupt Flag */
        valid = TRUE;
      }
    
    if( HAL_KEY_SW_1_PXIFG & HAL_KEY_SW_1_BIT) /* Interrupt Flag has been set by SW1 */
      {
        HAL_KEY_SW_1_PXIFG = ~(HAL_KEY_SW_1_BIT); /* Clear Interrupt Flag */
        valid = TRUE;
      }
  
    if( HAL_KEY_SW_2_PXIFG & HAL_KEY_SW_2_BIT) /* Interrupt Flag has been set by SW2 */
      {
        HAL_KEY_SW_1_PXIFG = ~(HAL_KEY_SW_2_BIT); /* Clear Interrupt Flag */
        valid = TRUE;
      }
  
  if (valid)
  {
    osal_start_timerEx (Hal_TaskID, HAL_KEY_EVENT, HAL_KEY_DEBOUNCE_VALUE);
  }
}

/**************************************************************************************************
 * @fn      HalKeyEnterSleep
 *
 * @brief  - Get called to enter sleep mode
 *
 * @param
 *
 * @return
 **************************************************************************************************/
void HalKeyEnterSleep ( void )
{
}

/**************************************************************************************************
 * @fn      HalKeyExitSleep
 *
 * @brief   - Get called when sleep is over
 *
 * @param
 *
 * @return  - return saved keys
 **************************************************************************************************/
uint8 HalKeyExitSleep ( void )
{
  /* Wake up and read keys */
  return ( HalKeyRead () );
}

/***************************************************************************************************
 *                                    INTERRUPT SERVICE ROUTINE
 ***************************************************************************************************/

/**************************************************************************************************
 * @fn      halKeyPort0Isr
 *
 * @brief   Port0 ISR
 *
 * @param
 *
 * @return
 **************************************************************************************************/
//HAL_ISR_FUNCTION( halKeyPort1Isr, P1INT_VECTOR )
//{
//  HAL_ENTER_ISR();
//
//  if ((HAL_KEY_BOOT_PXIFG & HAL_KEY_BOOT_BIT))
//      
//  {
//    halProcessKeyInterrupt();
//  }
//
//  /*
//    Clear the CPU interrupt flag for Port_0
//    PxIFG has to be cleared before PxIF
//  */
//
//    HAL_KEY_BOOT_PXIFG = 0;
//  
//    HAL_KEY_CPU_PORT_1_IF = 0;
//
//  CLEAR_SLEEP_MODE();
//
//  HAL_EXIT_ISR();
//
//  return;
//}


/**************************************************************************************************
 * @fn      halKeyPort2Isr
 *
 * @brief   Port2 ISR
 *
 * @param
 *
 * @return
 **************************************************************************************************/
HAL_ISR_FUNCTION( halKeyPort0Isr, P0INT_VECTOR )
{
  HAL_ENTER_ISR();

  if (( HAL_KEY_SW_1_PXIFG & HAL_KEY_SW_1_BIT) ||
      ( HAL_KEY_SW_2_PXIFG & HAL_KEY_SW_2_BIT) ||
       (HAL_KEY_BOOT_PXIFG & HAL_KEY_BOOT_BIT))
  {
    halProcessKeyInterrupt();
  }

  /*
    Clear the CPU interrupt flag for Port_2
    PxIFG has to be cleared before PxIF
    Notes: P2_1 and P2_2 are debug lines.
  */
    HAL_KEY_SW_1_PXIFG = 0;
    HAL_KEY_SW_2_PXIFG = 0;
    HAL_KEY_BOOT_PXIFG = 0;
  
  HAL_KEY_CPU_PORT_0_IF = 0;
  
  CLEAR_SLEEP_MODE();

  HAL_EXIT_ISR();

  return;
}
//#endif
#else

void HalKeyInit(void){}
void HalKeyConfig(bool interruptEnable, halKeyCBack_t cback){}
uint8 HalKeyRead(void){ return 0;}
void HalKeyPoll(void){}

#endif
/**************************************************************************************************
**************************************************************************************************/
