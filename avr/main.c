/*
 * Written in the end of April 2020 by Niklas Ekström.
 */
#include <avr/io.h>
#include <avr/interrupt.h>

// Amiga       AVR pins    AVR type         AVR ports    SPI pins    SD pins
// -----       --------    --------         ---------    --------    -------
// D0          A0          BOTH             PC0
// D1          A1          BOTH             PC1
// D2          A2          BOTH             PC2
// D3          A3          BOTH             PC3
// D4          A4          BOTH             PC4
// D5          A5          BOTH             PC5
// D6          D6          BOTH             PD6
// D7          D7          BOTH             PD7

// BUSY/IDLE   D4          OUTPUT           PD4
// POUT/CLOCK  D5          INPUT            PD5

// SEL         --          --               --           CS
// STROBE      --          --               --           --      

//             D10         OUTPUT           PB2          SS'         CD/DAT3
//             D11         OUTPUT           PB3          MOSI        CMD
//             D12         INPUT            PB4          MISO        DAT0
//             D13         OUTPUT           PB5          SCK         CLK

// CD'         D8          INPUT_PULLUP     PB0 ---|
// ACK         D9          OUTPUT           PB1 <--|


// INFORMATION:
// ------------
// AVR generates FLG interrupts on Amiga's parallel port, based on the CD' signals from the MicroSD socket as soon as a MicroSD is inserted/ejected
//  -> converts H->L (insert card) and L->H (eject card) to Amiga compatible H->L signals (Amiga triggers interrupt on ACK line only on falling edge H->L)


// Port B: Serial Peripheral Interface (SPI)
#define SCK_BIT     5
#define MISO_BIT    4
#define MOSI_BIT    3
#define SS_BIT      2

// Port B: Card Detect (CD)
#define CD_BIT      0                                                           // Reads state of CD' pin on MicroSD adapter (pulled to GND if card is inserted)
#define ACK_BIT     1                                                           // Propagates CD' state to Amiga via Parallel port

// Port D: Parallel Port Control Lines
#define IDLE_BIT    4
#define CLOCK_BIT   5

// ISR called on every a state change at PB0/CD'
ISR(PCINT0_vect) {
    if(PINB & (1 << CD_BIT))                                                    // Invert and propagate CD' bit to ACK interrupt line.
        PORTB &= ~(1 << ACK_BIT);                                               //   Set ACK=0 if CD'=1
    else
        PORTB |= (1 << ACK_BIT);                                                //   Set ACK=1 if CD'=0
}

int main() {

    // Configure SPI bus
    DDRB = (1 << SCK_BIT) | (1 << MOSI_BIT) | (1 << SS_BIT) | (1 << ACK_BIT);   // Set SCK, MOSI and SS as OUTPUT and MISO as INPUT, ACK as OUTPUT
    PORTB = (1 << SS_BIT) | (1 << CD_BIT);                                      // Set SS to HIGH (Chip Select), CD' to INPUT_PULLUP and ACK to LOW

    // SPI enabled, master, fosc/64 = 250 kHz
    SPCR = (1 << SPE) | (1 << MSTR) | (1 << SPR1) | (1 << SPR0);                // SPI enabled, master, fosc/64 = 250 kHz   <= Isn't this fosc/128 when SPR0=1 && SPR1=1 ???
    SPSR |= (1 << SPI2X);                                                       // SPI is doubled when the SPI is in Master mode.

    DDRC = 0;                                                                   
    PORTC = 0;                                                                  

    DDRD = (1 << IDLE_BIT);                                                     
    PORTD = 0;                                                                  

    // Configure interrupts
    PCICR |= (1 << PCIE0);                                                      // Set PCIE0 (enables PCINT0..7) to enable PCMSK0 scan
    PCMSK0 |= (1 << PCINT0);                                                    // Enable PCINT0 (PB0/D8) to trigger an interrupt on any state change
    sei();                                                                      // Turn interrupts on

    uint8_t pin_c;                                                              
    uint8_t pin_d;                                                              
    uint8_t next_port_d;                                                        
    uint8_t next_port_c;                                                        
    uint16_t byte_count;                                                        

main_loop:

    if (PIND & (1 << CLOCK_BIT))                                            
        while (PIND & (1 << CLOCK_BIT));                                        
    else
        while (!(PIND & (1 << CLOCK_BIT)));                                     

    if (!(PIND & 0b10000000)) {                                                 
        if (PIND & 0b01000000)                                                  
            goto do_read1;
        else                                                                    
            goto do_write1;
    }

    pin_c = PINC;                                                               
    pin_d = PIND;                                                               
    
    // READ2 or WRITE2
    if (!(pin_d & 0b01000000)) {                                                
        byte_count = (pin_c & 0b00011111) << 8;                                 

        if (pin_d & (1 << CLOCK_BIT))
            while (PIND & (1 << CLOCK_BIT));                                    
        else
            while (!(PIND & (1 << CLOCK_BIT)));                                 

        pin_d = PIND;                                                           

        byte_count |= (pin_d & 0b11000000) | PINC;                              
        //byte_count = byte_count | (pin_d & 0b11000000) | PINC;

        if (pin_c & 0b00100000)                                                 
            goto do_read;
        else
            goto do_write;

    } else if ((pin_c & 0b00111110) == 0) {                                     
        if (pin_c & 1)                                                          
            SPCR = (1 << SPE) | (1 << MSTR);
        else                                                                    
            SPCR = (1 << SPE) | (1 << MSTR) | (1 << SPR1) | (1 << SPR0);
    }

    goto main_loop;

do_read1:

    byte_count = PINC;

do_read:

    SPDR = 0b11111111;                                                          

    pin_d = PIND;                                                               

    PORTD = (pin_d & 0b11000000) | (1 << IDLE_BIT);                             
    DDRD = 0b11000000 | (1 << IDLE_BIT);                                        

    PORTC = byte_count & 0b00111111;                                            
    DDRC = 0b00111111;                                                          

read_loop:                                                                      

    while (!(SPSR & (1 << SPIF)));

    next_port_c = SPDR;
    next_port_d = (next_port_c & 0b11000000) | (1 << IDLE_BIT);

    if (pin_d & (1 << CLOCK_BIT))
        while (PIND & (1 << CLOCK_BIT));
    else
        while (!(PIND & (1 << CLOCK_BIT)));

    PORTD = next_port_d;
    PORTC = next_port_c;

    pin_d = PIND;

    if (byte_count) {
        byte_count--;
        SPDR = 0b11111111;
        goto read_loop;
    }

    if (pin_d & (1 << CLOCK_BIT))
        while (PIND & (1 << CLOCK_BIT));
    else
        while (!(PIND & (1 << CLOCK_BIT)));

    DDRD = (1 << IDLE_BIT);
    DDRC = 0;

    PORTD = 0;
    PORTC = 0;

    goto main_loop;

do_write1:

    byte_count = PINC;
    pin_d = PIND;

do_write:

    PORTD = (1 << IDLE_BIT);                                                    

write_loop:                                                                     

    if (pin_d & (1 << CLOCK_BIT))
        while (PIND & (1 << CLOCK_BIT));                                        
    else
        while (!(PIND & (1 << CLOCK_BIT)));                                     

    pin_d = PIND;
    SPDR = (pin_d & 0b11000000) | PINC;

    while (!(SPSR & (1 << SPIF)));

    (void) SPDR;

    if (byte_count) {
        byte_count--;
        goto write_loop;
    }

    PORTD = 0;

    goto main_loop;

    return 0;

}