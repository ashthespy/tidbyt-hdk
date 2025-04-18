


#ifdef TIDBYT_GEN2
#define R1 5
#define G1 23
#define BL1 4
#define R2 2
#define G2 22
#define BL2 32

#define CH_A 25
#define CH_B 21
#define CH_C 26
#define CH_D 19
#define CH_E -1  // assign to pin 14 if using more than two panels

#define LAT 18
#define OE 27
#define CLK 15
#elif defined(TIXEL)
#pragma message "Compiling for TIXEL board pins"
#define PIN_BUTTON_1 36
#define PIN_BUTTON_2 39
#define PIN_BUTTON_3 34
#define PIN_BUTTON_4 35

#define LED_MATRIX_MOSFET GPIO_NUM_4
#define R1 22
#define G1 32
#define BL1 21
#define R2 19
#define G2 33
#define BL2 17

#define CH_A 16
#define CH_B 25
#define CH_C 27
#define CH_D 26
#define CH_E -1

#define LAT 14
#define OE 13
#define CLK 12
#elif defined(ESPS3)
#pragma message "Compiling for ESPS3 board pins"
#define LED_MATRIX_MOSFET GPIO_NUM_13
#define R1 4
#define G1 6
#define BL1 5
#define R2 7
#define G2 16
#define BL2 15

#define CH_A 17
#define CH_B 18
#define CH_C 8
#define CH_D 3

#define LAT 9
#define OE 10
#define CLK 11
#define CH_E -1  // assign to pin 14 if using more than two panels
// Default Matrix lib pinmap
// #define R1 4          
// #define G1 5          
// #define BL1 6         
// #define R2 7          
// #define G2 15         
// #define BL2 16        

// #define CH_A 18       
// #define CH_B 8        
// #define CH_C 3        
// #define CH_D 42       

// #define LAT 40        
// #define OE 2          
// #define CLK 41        
// #define CH_E -1       
#else
#define R1 21
#define G1 2
#define BL1 22
#define R2 23
#define G2 4
#define BL2 27

#define CH_A 26
#define CH_B 5
#define CH_C 25
#define CH_D 18
#define CH_E -1  // assign to pin 14 if using more than two panels

#define LAT 19
#define OE 32
#define CLK 33
#endif




