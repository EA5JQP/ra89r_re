

<!-- Start of picture text -->
Lean<br>_> BEKEN<br>—_24<br><!-- End of picture text -->

# **<u>BK4815N</u>** 

## **_Analog Two Way Radio IC_** 

### **Features** 

-  World wide band: 128 ~ 590 MHz 

-  12.5/25 kHz channel spacing 

-  On chip 5 dBm RF PA 

-  3.0 V to 3.6 V power supply 

-  CTCSS tone receiver with up to parallel eight frequency detector 

-  23/24 bit programmable DCS code 

-  Standard DTMF and programmable in-band dual tone 

-  SELCALL and programmable in-band single tone 

-  1.2/2.4 kbps FSK data modem with either F2D or F1W modulation type 

-  Frequency inversion scrambler 

-  Voice activated switch (VOX) and time-out timer 

-  RF Signal strength measurement and signal quality measurement 

-  TX Audio signal strength indication and RX audio signal strength indication 

-  3-wires interface with MCU with maximum 8 Mbps clock rate 

-  QFN 4x4 32-Pin package 

### **Applications** 

 Personal Radio Service  Personal Radio Service ~~7 oD~~  Baby Monitor  Baby Monitor 

-  Personal Radio Service  Personal Radio Service 

-  Baby Monitor  Baby Monitor 

-  Toys 

### **<u>General Description</u>** 

The BK4815N is a half duplex TDD FM transceiver operating from 124 MHz to 560 MHz band for worldwide personal radio. Besides speech communication, the BK4815N on-chip FSK data modem supports F2D and F1W emission to be used in both FRS and DPMR band for text message and GPS information exchange. 

The BK4815N is a complete, small form factor solution optimized for low-power, low-cost, and highly integrated mobile and portable consumer electronic devices, requiring only a few external decoupling capacitors and an external inductor for input matching. 



<!-- Start of picture text -->
32 31 30 29 28 27 26 25<br>IFQN 1 24 GNDDIG<br>IFQP 2 23 VBATD<br>VCCXTAL 3 22 CDVDD<br>XO 4 BK4815N 21 VCCVCO<br>XI 5 QFN4x4 20 NC<br>VDDXTAL 6 19 NC<br>VCCCB 7 18 VRAMP<br>EARO 8 17 PAOUT<br>9 10 11 12 13 14 15 16<br>QFN 32 Pin Assignments (Top View)<br>IFIN IFIP INTN GPIO1 GPIO0 SDATA SCN SCK<br>NC NC VCCPLL VCCIF MICN MICP LNAIN VCCRF<br><!-- End of picture text -->

### **Functional Block Diagram** 



<!-- Start of picture text -->
cTCss Transmitter=<br>CDCSS<br>DSP Re<br>SELCALL Synthesizer<br>DTMF RF<br><!-- End of picture text -->

**Rev.1.0 Copyright©2016 by Beken Corporation** Beken Confidential. Information contained herein is covered under non-disclosure agreement (NDA). 

**BK4815N** 



# **<u>BK4815N</u>** 

### **Table of Contents** 

|**1**<br>**Functional Description ......................................................................................................... 4**|
|---|
|1.1<br>Overview ........................................................................................................................ 4|
|1.2<br>RF Transceiver ............................................................................................................... 4|
|_1.2.1_<br>_FM Receiver_............................................................................................................... 4|
|_1.2.2_<br>_FM Transmitter_........................................................................................................... 5|
|_1.2.3_<br>_RF Frequency Synthesizer_........................................................................................ 6<br><br>|
|_1.2.4_<br>_Input/output Matching_................................................................................................ 7|
|_1.2.5_<br>_Crystal Oscillator_........................................................................................................ 7|
|_1.2.6_<br>_On-chip Voltage Regulator Decoupling_..................................................................... 8|
|_1.2.7_<br>_Power Supply Decoupling_.......................................................................................... 9|
|1.3<br>TX Baseband .................................................................................................................. 9|
|_1.3.1_<br>_Audio_.......................................................................................................................... 9|
|_1.3.2_<br>_VOX_.......................................................................................................................... 11|
|_1.3.3_<br>_In-band Signaling_..................................................................................................... 11|
|_1.3.4_<br>_Sub-audible Signaling_.............................................................................................. 13|
|1.4<br>RX Baseband ............................................................................................................... 14|
|_1.4.1_<br>_Audio_........................................................................................................................ 14|
|_1.4.2_<br>_In-band Signaling_..................................................................................................... 14|
|_1.4.3_<br>_Sub-audible Signaling_.............................................................................................. 15|
|1.5<br>SPI Interface ................................................................................................................. 15|
|1.6<br>Power Management ..................................................................................................... 16|
|**2**<br>**Electrical Specifications..................................................................................................... 17**|
|2.1<br>Absolute Maximum Ratings ......................................................................................... 17|
|2.2<br>Recommended Operating Conditions .......................................................................... 17|
|2.3<br>Power Consumption Specification ............................................................................... 17|
|2.4<br>Receiver Characteristics .............................................................................................. 18|
|2.5<br>Transmitter Characteristics .......................................................................................... 19|
|2.6<br>SPI Control Interface Characteristics ........................................................................... 20|
|**3**<br>**Register Definition .............................................................................................................. 21**|
|**4**<br>**Pin Assignment ................................................................................................................... 34**|
|**5**<br>**Typical Application Schematic .......................................................................................... 36**|
|**6**<br>**Package Information ........................................................................................................... 37**|
|**7**<br>**Tape and Reel Information ................................................................................................. 40**|
|7.1<br>Tape Information .......................................................................................................... 40|
|7.2<br>Cover Information ......................................................................................................... 41|
|7.3<br>Reel Information ........................................................................................................... 42|
|**8**<br>**Solder Reflow Profile .......................................................................................................... 43**|
|**9**<br>**Order Information ............................................................................................................... 44**|
|**10**<br>**Additional Reference Resources ...................................................................................... 45**|
|**11**<br>**Revision History .................................................................................................................. 46**|
|**12**<br>**Contact Information ............................................................................................................ 47**|



**Copyright©2016 by Beken Corporation** 

**Rev.1.0 Copyright©2016 by Beken Corporation** Beken Confidential. Information contained herein is covered under non-disclosure agreement (NDA). 

**BK4815N** 



# **<u>BK4815N</u>** 

###### **Figure List** 

|Figure 1. Functional Block Diagram ......................................................................... 4|
|---|
|Figure 2. Radio Block Diagram ................................................................................ 5|
|Figure 3. Frequency Cover Range of RF Synthesizer .............................................. 6|
|Figure 4. Schematic of Input/Output matching ......................................................... 7|
|Figure 5. Circuit Diagram of the 26MHz Crystal Oscillator ....................................... 8|
|Figure 6. Configuration of Low-dropout Regulators .................................................. 9|
|Figure 7. TX Audio Block Diagram ........................................................................... 9|
|Figure 8. Frequency Response of Pre-emphasis Filter .......................................... 10|
|Figure 9. Frequency Response of 300Hz High Pass Filter ..................................... 10|
|Figure 10. Frequency Response of Low Pass Filter ............................................... 11|
|Figure 11. Frame Structure of Data Mode.............................................................. 12|
|Figure 12. TX Audio Block Diagram ....................................................................... 14|
|Figure 13. Three-wires Interface Timing ................................................................ 16|
|Figure 14. 3-Wire Control Interface Write Timing Diagram ..................................... 20<br>|
|Figure 15. 3-Wire Control Interface Read Timing Diagram .................................... 20|
|Figure 16. BK4815N Pin Assignment (Top View)................................................... 34|
|Figure 17. BK4815N Application ............................................................................ 36|
|Figure 18. QFN 4x4 32 Pin Package diagram ........................................................ 37|
|Figure 19. Tape Dimensions .................................................................................. 40|
|Figure 20. Cover Dimensions ................................................................................ 41|
|Figure 21. Reel Dimensions .................................................................................. 42|
|Figure 22. Classification Reflow Profile .................................................................. 43|



###### **Table List** 

|Table 1. Standard DTMF Table ............................................................................. 12|
|---|
|Table 2. EIA single tone frequency setting ............................................................. 13|
|Table 3. Reference CTCSS Frequency ................................................................. 13|
|Table 4. Power Control for Different Operational States ........................................ 16|
|Table 5. Absolute Maximum Ratings ..................................................................... 17|
|Table 6. Recommended Operating Conditions ...................................................... 17|
|Table 7. Power Consumption Specification ............................................................ 17|
|Table 8. Receiver Characteristics .......................................................................... 18|
|Table 9. Transmitter Characteristics ...................................................................... 19|
|Table 10. SPI Control Interface Characteristics ..................................................... 20|
|Table 11. Interface Register Definition ................................................................... 21|
|Table 12. BK4815N 4mmx4mm 32-Pin Definition .................................................. 34|
|Table 13. QFN 4x4 32 Pin Package dimensions .................................................... 38|



**Copyright©2016 by Beken Corporation** 

**Rev.1.0 Copyright©2016 by Beken Corporation** Beken Confidential. Information contained herein is covered under non-disclosure agreement (NDA). 

**BK4815N** 

**BK4815N** 



### **1 Functional Description** 



<!-- Start of picture text -->
CTCCS<br>MUX<br>CDCCS  Ramp DAC<br>Time out Timer  SELCALL<br>26  MHz<br>(TOT) DTMF PLL<br>MUX MUX Modulator<br>FSK AIR<br>Voice Activate<br>AGC Switch<br>FSK Mod.<br>ADC Compress Emphasis Scrambling Transmitter<br>ADC<br>Filter<br>Demod De- De-<br>RSSI Emphasis Scrambling Expander DAC<br>SNR<br>ADC CTCCS<br>LPF<br>CDCCS  Squelch<br>PLL SELCALL<br>DTMF<br>MISC M<br>BPF FSK Demod. SPI C<br>U<br>Receiver FSK AIR<br><!-- End of picture text -->

**Figure 1. Functional Block Diagram** 

#### **_1.1 Overview_** 

The BK4815N integrates high performance PLL, ADC, DAC, and advanced digital signal processing capability on a single chip. The digital low-IF image rejection architecture enables it to work with a very simple MCU as a two way radio communication system. On-chip flexible and precise continuous and discrete tone generator and detector enable a secure link and digital signaling. 

#### **_1.2 RF Transceiver_** 

BK4815N includes an integrated RF transceiver which is compliant with the specification most country in the world. The RF transceiver requires the following external components to operate: 

- 1) A 26MHz crystal; 

- 2) Simple input matching and output matching; 

- 3) Several SMD capacitors for decoupling and DC blocking. 

##### **_1.2.1 FM Receiver_** 

The receiver implements a low-IF image rejection architecture, which is composed with two parts: RF front-end and IF part. The RF front-end comprises a LNA and a quadrature mixer. The IF part comprises a low-pass filter (LPF) for channel filtering, a variable gain amplifier (VGA) and a high precision analog-to- 

Proprietary and Confidential 

Page 4 of 47 

**BK4815N** 



digital converter(ADC). The block diagram of the FM receiver is shown in Figure 2. 

At the RF front-end part, the LNA is a differential low-noise amplifier with singleended input. The LNA is followed by a quadrature mixer that down-converts the RF signal directly to IF signal. Low-IF image rejection architecture is implemented in order to eliminate the external SAW filters. The value of IF frequency( _fIF_ ) can be programmed either 88kHz or 137kHz through 3-wire SPI interface( _REG13[0]_ ). The RF front-end part can be power up by set _REG12[14]_ to 0. 

At the IF part, the down-converted in-phase IF signal (IF/I) and quadrature-phase IF signal(IF/Q)are first filtered by the BPF, and then amplified by the VGA. The VGA provides variable gain with 21dB dynamic range, and could be controlled through3-wire SPI interface( _REG103[6:4]_ ).The Sigma-Delta ADC sample analog IF signal from VGA, and convert it to digital IF signal. Then the digital signal will be send to DSP for second down-conversion and audio processing. The IF part can be power up by set _REG12[13]_ to 0. 

To avoid serious distortion with high-level input power, AGC function is added to automatically adjust the gain of LNA and the gain of VGA. AGC function can be enabled by set _REG103[15]_ to 0. 



<!-- Start of picture text -->
LNA Mixer Filter + VGA + ADC<br>IF/I Do/I<br>Rfin<br>IF/Q Do/Q<br>FM Receiver<br>DIV LF CP<br>Ramping<br>VCO<br>Rfout Frac-N PFD<br>- Reference Clock<br>Mod from OSC<br>Buffer + PA Driver<br>FM Transmitter RF Frequency Synthesizer<br><!-- End of picture text -->

**Figure 2. Radio Block Diagram** 

##### **_1.2.2 FM Transmitter_** 

The transmitter is a single-ended amplifier including a buffer, a PA driver and a ramping-control block. The block diagram of the transmitter is shown in Figure 2. Due to FM modulation is of constant envelope, the amplifier works in saturated mode to save current consumption. 

Proprietary and Confidential 

Page 5 of 47 

**BK4815N** 



A ramping-control block is implemented to avoid unwanted spurious signals when the transmitter is power up. Ramping function can be enabled by set _REG109[12]_ to 1. If ramping function is enabled, a ramping table should be filled by writing ramp values into _REG42_ in turn. The best ramping curve is raised cosine. 

The output power of FM transmitter can be programmed from -20dBm to +5dBm through 3-wire SPI interface( _REG109[15:13]_ ). 

The transmitter can be power up by set _REG12[9]_ to 0. 

##### **_1.2.3 RF Frequency Synthesizer_** 

An RF synthesizer is implemented to generate local oscillator(LO) signals. It includes a voltage controlled oscillator(VCO), a fractional-N divider(frac-N), a phase-frequency detector(PFD), a charge pump(CP) and a loop filter(LF). The RF synthesizer is shared for RX mode and TX mode. The block diagram of the synthesizer is shown in Figure 2. 

In RX mode, the RF frequency synthesizer generates unmodulated LO signal. And the unmodulated LO signal is then divided by an integer _Ndiv_ for downconversion mixer in the FM receiver. In TX mode, FM modulation is realized in the RF frequency synthesizer. Modulated VCO output is divided by an integer _Ndiv_ . The value of _Ndiv_ can be programmed to 8/12/16/24 through 3-wire SPI interface( _REG4[8:7]_ ). 



<!-- Start of picture text -->
Band4<br>Band3<br>Band2<br>Band1<br>100 200 300 400 500 600<br>Frequency (MHz)<br><!-- End of picture text -->

**Figure 3. Frequency Cover Range of RF Synthesizer** 

In RX mode, the locked frequency of the synthesizer is equal to _Ndiv_ x ( _fwanted_ - _fIF_ ). While in TX mode, the locked frequency of the synthesizer is equal to _Ndiv_ x _fwanted_ . 

Channel selection is also implemented by programming the value of fraction-N through 3-wire SPI interface ( _REG113/REG114_ ). On power up or channel reselection, the synthesizer takes less than 0.3m sec to settle. 

For BK4815N, the default crystal is 26MHz. The frequency tolerance of the crystal should be within 2.5ppm to keep a reliable communication. 

The synthesizer can be power up by set _REG12[8]_ to 0. 

Proprietary and Confidential 

Page 6 of 47 

**BK4815N** 



<!-- Start of picture text -->
~ SN<br>“am<br><!-- End of picture text -->

##### **_1.2.4 Input/output Matching_** 

Since the LNA input and the PA output are of single-ended, external balun is not necessary. Both the input matching and the output matching can be implemented using low-cost discrete inductors and capacitors. The schematic of input/output matching is shown in Figure 4. 



<!-- Start of picture text -->
C2<br>TX Port 17 PAOUT<br>C1<br>RX Port 15 LNAIN<br>L1<br><!-- End of picture text -->

**Figure 4. Schematic of Input/Output matching** 

As for input matching, capacitor C1 is used for DC-blocking. The recommended value of C1 is 4.7pF. The DC voltage at PIN15 is about 0V in RX mode. Inductor L1 is used for impedance transformation. The recommended value of L1 is 47nH for 409.75MHz band, and is 33nH for 446.00625MHz/462.5625MHz band. 

As for output matching, capacitor C2 is used for DC-blocking, too. 

##### **_1.2.5 Crystal Oscillator_** 

BK4815N integrates a low-power amplitude-regulated 26MHz crystal oscillator. The 26MHz crystal oscillator not only provides the reference frequency for the RF synthesizer, but also provides clock for digital part. The circuit diagram of the 26MHz crystal oscillator is shown in Figure 5. 

The 26 MHz crystal oscillator is designed for use with a quartz crystal in parallel resonant mode. To achieve correct oscillation frequency, the load capacitance must match the specification in the crystal datasheet. Figure 5 on next page shows how the crystal is connected to the 26 MHz crystal oscillator. C1 and C2 are ceramic SMD (Surface Mount Device) capacitors connected between each crystal terminal and ground. Cvar is an adjustable capacitor for frequency calibration. 

Proprietary and Confidential 

Page 7 of 47 

**BK4815N** 

<u>— ff</u> 



<!-- Start of picture text -->
Current Source for<br>' vee Amplitude Control vee H'<br>'<br>''<br>| ESD ESD}<br>''<br>''<br>' vis | vss '<br>''<br>'<br>'XTAL OSC Core Buffer |<br>(PIND (PINS)<br>Crystal<br>Cvar t c1 L c2<br><!-- End of picture text -->

**Figure 5. Circuit Diagram of the 26MHz Crystal Oscillator** 

in which, Cpar is parasitic capacitance including PCB trace capacitance and pin input capacitance. The value of Cpar is about 1pF. 

The 26MHz crystal oscillator can be power up by set _REG12[6]_ to 0. 

##### **_1.2.6 On-chip Voltage Regulator Decoupling_** 

BK4815N contains 2 low-dropout regulators (LDO): 

- 1) One LDO for XTAL oscillator in FM receiver. This LDO needs an external SMD capacitor to reduce noise on power supply for better noise figure. The output voltage of this LDO is about 1.2V, and the typical value of this SMD capacitor is 100nF. 

- 2) One LDO for digital part. This LDO needs an external SMD capacitor to reduce spurious signal from digital part. The output voltage of this LDO is about 1.2V, and the value of this SMD capacitor should be no less than 1uF. 

The configuration of these 2 LDOs is shown in Figure 6. 

Proprietary and Confidential 

Page 8 of 47 

**BK4815N** 





<!-- Start of picture text -->
BK4815N<br>VCCXTAL<br>VBATD<br>(PIN3)<br>(PIN23)<br>LDO for  LDO for<br>VDDXTAL XTAL OSC Digital CDVDD<br>(PIN6) (PIN22)<br>100nF >1uF<br>Figure 6. Configuration of Low-dropout Regulators<br><!-- End of picture text -->

##### **_1.2.7 Power Supply Decoupling_** 

Proper power-supply decoupling must be used for optimum performance. The placement and size of the decoupling capacitors and the power supply filtering are very important to achieve the best performance in an application. 



#### **_1.3 TX Baseband_** 

##### **_1.3.1 Audio_** 

TX audio path has the following blocks: 

- 1) Digital AGC to automatically adjust microphone gain ( _REG44, REG45_ ) before ADC; 

- 2) Digital compressor to extent signal dynamic range ( _REG17_ ); 

- 3) Volume control with 1 dB step from -25 dB to 6 dB ( _REG18_ ); 

- 4) Optional pre-emphasis filter, 0 dB at 1 kHz and +6 dB per octave (REG18); 

- 5) Audio scrambling with programmable scrambling frequency (REG20); 

- 6) 300 Hz high pass filter to avoid interference to sub-audible signal, which has 30 dB attenuation for frequency below 250 Hz with respect to signal at 1 kHz ( _REG18_ ); 

- 7) Low pass filter with typical 3 kHz corner ( _REG18_ ); 

- 8) Limiter to avoid unwanted out-of-band emission ( _REG19_ ); 

- 9) Flexible block execution order ( _REG18_ ). 



<!-- Start of picture text -->
Volume Pre-Emphasis Scrambling HPF LPF Limiter<br><!-- End of picture text -->

**Figure 7. TX Audio Block Diagram** 

Proprietary and Confidential 

Page 9 of 47 

**BK4815N** 



The microphone gain can be control through _REG44_ and _REG45_ . 

When AGC is enabled, the microphone gain is automatically controlled by internal AGC algorithm. At this scenario, the real time microphone gain setting can be read by _REG21[5:0]_ . Combined with the real time microphone gain ( _REG21 [5:0]_ ) and the digital detected microphone signal level ( _REG21_ ), the absolute microphone signal level at microphone input can be calculated, but please note the _REG21[15:8]_ and _REG21[5:0]_ have difference unit. 



<!-- Start of picture text -->
p q y p<br>15<br>10<br>5<br>0<br>-5<br>-10<br>-15<br>-20<br>-25<br>-30<br>-35<br>1 2 3 4 5<br>10 10 10 10 10<br>Frequency (Hz)<br>Figure 8. Frequency Response of Pre-emphasis Filter<br>10<br>0<br>-10<br>-20<br>-30<br>-40<br>-50<br>-60<br>-70<br>-80<br>-1 0 1 2 3 4 5<br>10 10 10 10 10 10 10<br>Frequency (Hz)<br>Amplitdue (dB)<br>Amplitdue (dB)<br><!-- End of picture text -->

**Figure 9. Frequency Response of 300Hz High Pass Filter** 

Proprietary and Confidential 

Page 10 of 47 

**BK4815N** 

<u>— 2</u> 



<!-- Start of picture text -->
3 kHz LPF Frequency Response<br>20<br>0<br>-20<br>-40<br>-60<br>-80<br>-100<br>-120<br>0 1000 2000 3000 4000 5000 6000 7000 8000 9000 10000<br>Frequency (Hz)<br>Amplitdue (dB)<br><!-- End of picture text -->

**Figure 10. Frequency Response of Low Pass Filter** 

##### **_1.3.2 VOX_** 

Voice activated switch (VOX) detects the received audio signal level ( _REG21[7:0]_ ) and microphone PGA gain ( _REG21[5:0]_ ). If the received audio signal level is greater than a programmable threshold ( _REG22[13:6]_ ) and the microphone PGA gain is greater than a programmable threshold ( _REG22[5:0]_ ), and this status last for 128 samples, the VOX will output 1 and the VOX interrupt bit will be set ( _REG116_ ). VOX can be enabled by set _REG22[15]_ to 1. 

##### **_1.3.3 In-band Signaling_** 

There are three types of in-band signaling: DTMF, SELCALL, and FSK (and FSK_AIR). Together with audio signal, they are total of four kinds of in-band signaling, of which only one can be sent at a time ( _REG40_ ). The deviation of inband signaling is programmable ( _REG40_ ). 

##### **_1.3.3.1 DTMF_** 

DTMF is a dual tone signaling, it has programmable high band and low band frequency. The high band frequency can be programmable from 1209 Hz to 1633 Hz ( _REG25_ ). The low band frequency can be programmable from 697 Hz to 941 Hz ( _REG24_ ). The twist can be programmable from 0 to 15 dB with 1 dB resolution ( _REG26_ ).  The suggested DTMF tone table is given below. 

Proprietary and Confidential 

Page 11 of 47 

**BK4815N** 



**Table 1. Standard DTMF Table** 

|DTMF S|bl||High Freq|uency (Hz)||
|---|---|---|---|---|---|
||mo|1209|1336|1477|1633|
|L|697|1|2|3|A|
|ow<br>F|770|4|5|6|B|
|requency<br>H|852|7|8|9|C|
|(z)|941|E|0|F|D|



##### **_1.3.3.2 FSK_** 

FSK is a high data rate signaling, which supports 1200 bps data mode at 12.5 kHz channel spacing and 2400 bps data mode at 25 kHz channel spacing. The frame structure of the data package is given below. 

|**Pre-amble**|**Sync**<br>**Word**|**Addr**|**Type**|**Size**|**CRC**<br>**A**|**Payload**|**CRCB**|
|---|---|---|---|---|---|---|---|
|**16 bit**|**16 bit**|**Byte0**|**Byte1**|**Byte2**|**Byte3**|**0-127 Word**|**2/4 Byte**|



###### **Figure 11. Frame Structure of Data Mode** 

User can write head field including Address/Type/Size/CRCA and the corresponding payload ( _REG28, REG30_ ), and it will automatically calculate the CRC and packetize the data. Optional scrambling can be added to Address and subsequent bytes, and the scrambling seed is programmable ( _REG32_ ). 

Data receiver will automatically finish synchronization and data extraction that Address/Type/Size/CRCA and Payload can be read out through MCU interface. 

The final over the air data package type can be setting with Type byte. 

- 1) Type 0: Only head, no payload 

- 2) Type 1: Head with payload 

- 3) Type 2: Head with FEC encoded payload 

- 4) Type 3: Head with FEC and interleaved encoded payload 

- 5) Type 4: Free format that no automatic CRC insertion, CRCA is a user writable byte 

The payload write is through an 8 words (1 word = 2 bytes) FIFO, if the word number in FIFO is shorter than a threshold that a write operation requires, it will give an interrupt to MCU that MCU must refill the FIFO (REG31). 

Note: if use type 3, the number of payload is restricted. The allowed payload number is either odd number less than 8 or even number greater than 9. Payload number 8 and 9 is not allowed for type 3. 

Proprietary and Confidential 

Page 12 of 47 

**BK4815N** 



The FSK packet can be transmitted either directly through FM modulation (FSK AIR) for DPMR band or with a MSK modulated sub-carrier (FSK modem) then to FM modulation for FRS band. 

##### **_1.3.3.3 SELCALL_** 

SELCALL is a single tone signaling, the frequency can be programmable from 400 Hz to 3000 Hz ( _REG34_ ). To get high sensitivity the suggested tone frequency (EIA frequency group) is given below. 

**Table 2. EIA single tone frequency setting** 

|Tone Number|0|1|2|3|4|5|6|7|
|---|---|---|---|---|---|---|---|---|
|Tone Frequency (Hz)|600|741|882|1023|1164|1305|1446|1587|
|Tone Number|8|9|A|B|C|D|E|F|
|Tone Frequency (Hz)|1728|1869|2151|2435|2010|2295|495|No Tone|



##### **_1.3.4 Sub-audible Signaling_** 

Sub-audible signaling includes both CTCSS and CDCSS. For CTCSS, the frequency is programmable with 18 bit resolution, and it can be set to have a 0/120/180 degree phase shift. For CDCSS, it supports user programmable 23/24 bits CDCSS code( _REG1[12], REG38, REG39_ ), and the transmitted CDCSS code can be inversed. 

**Table 3. Reference CTCSS Frequency** 

|Freq.<br>(Hz)|Freq. (Hz)|Stan<br>Freq. (Hz)|dard CTCSS<br>Freq. (Hz)|Tone<br>Freq. (Hz)|Freq. (Hz)|Freq. (Hz)|
|---|---|---|---|---|---|---|
|**67.0**|**85.4**|**103.5**|**127.3**|**156.7**|**192.8**|**241.8**|
|**71.9**|**88.5**|**107.2**|**131.8**|**162.2**|**203.5**|**250.3**|
|**74.4**|**91.5**|**110.9**|**136.5**|**167.9**|**210.7**||
|**77.0**|**94.8**|**114.8**|**141.3**|**173.8**|**218.1**||
|**79.7**|**97.4**|**118.8**|**146.2**|**179.9**|**225.7**||
|**82.5**|**100.0**|**123.0**|**151.4**|**186.2**|**233.6**||
|||Non-St|andard CTC|SS Tone|||
|Freq.<br>(Hz)|Freq. (Hz)|Freq. (Hz)|Freq. (Hz)|Freq. (Hz)|Freq. (Hz)|Freq. (Hz)|
|**62.5**|**69.3**|**183.5**|**196.6**|**206.5**|||
|**64.7**|**159.8**|**189.9**|**199.5**|**229.1**|||



Proprietary and Confidential 

Page 13 of 47 

**BK4815N** 



#### **_1.4 RX Baseband_** 

The RX baseband output is the FM demodulator output, whose amplitude can be scaled ( _REG66_ ), and subsequent filter coefficient is selected according to the inband signal type ( _REG66_ ) and sub-audible signal type ( _REG66_ ). 

##### **_1.4.1 Audio_** 

Audio path has blocks below. 

- 1) De-emphasis filter, 0 dB at 1 kHz and -6 dB per octave ( _REG72_ ); 

- 2) Audio de-scrambling with programmable scrambling frequency ( _REG72_ ); 

- 3) Low pass filter with 1.5 kHz corner ( _REG65[15:14]_ = 3) or 3.1 kHz corner ( _REG65[15:14]_ = 2); 

- 4) 15 dB volume control range with 1 dB per step ( _REG73[3:0]_ ); 

- 5) Hard mute or soft mute control based on receive signal quality ( _REG73_ ); 

- 6) Flexible block execution order ( _REG72_ ); 

**De-Scrambling De-Emphasis LPF HPF Volume Mute** 

###### **Figure 12. TX Audio Block Diagram** 

The RX signal strength before volume control module can be read out through REG74. 

##### **_1.4.2 In-band Signaling_** 

##### **_1.4.2.1 DTMF_** 

DTMF decoder can detect up to 16 DTMF symbols simultaneously ( _REG78_ ). High band tone frequency and low band tone frequency of each symbol can be programmed individually ( _REG77_ ). When the decoder finds a symbol match, it will set corresponding bit and the found symbol address ( _REG78_ ), and an interrupt will be issued. 

##### **_1.4.2.2 FSK_** 

In FSK AIR mode, the slicer output of FM demodulator will be FSK symbol. In FSK mode, the FM demodulator output is taken as a sub-carrier, and will be demodulated with a FSK demodulator. 

FSK receiver will search the sync word to establish synchronization with FSK transmitter. When it is synchronized and CRCA check is passed, a FSK head received interrupt ( _REG116_ ) will be issued and it will continue to receive the payload. The payload data will be written to an 8 words FIFO, and when the data word number in FIFO is greater than a threshold that the FIFO needs to read out, it will issue an interrupt ( _REG116_ ) and MCU should read out all data bytes in FIFO immediately ( _REG82, REG83_ ). After all payload of one packet is received, the CRCB check result will be set ( _REG83_ ) and a receivefinished interrupt will be issued to MCU that MCU should check this bit to 

Proprietary and Confidential 

Page 14 of 47 

**BK4815N** 



know whether the read out payload is valid or invalid that should be discarded. 

Note: With free format type (type 4), the ― FSK head received interrupt‖ will be given out immediately when it found the sync word , thus, this interrupt is earlier than that in other mode. 

##### **_1.4.2.3 SELCALL_** 

SELCALL decoder can detect up to 16 SELCALL symbols simultaneously ( _REG87_ ). Frequency of each symbol can be programmed individually ( _REG86_ ). When the decoder find a symbol match, it will set corresponding bit and the found symbol address ( _REG87_ ), and an interrupt will be issued. 

There is an optional high pass filter to filter out signal below 400 Hz ( _REG87_ ), and user can bypass this filter to receive SELCALL symbol with frequency below 400 Hz. 

##### **_1.4.3 Sub-audible Signaling_** 

CTCSS decoder can detect up to 8 CTCSS symbols simultaneously ( _REG92_ ). Frequency of each symbol can be programmed individually ( _REG91_ ). When the decoder find a symbol match, it will set corresponding bit and the found symbol address ( _REG92_ ), and an interrupt will be issued. 

_REG89 [15:4]_ to 0xFF8 can improve the sensitivity and stability, at the cost of much longer response time. In practical use, user can set the _REG89 [15:4]_ small to improve the response time, and after the CTCSS is found, user can change it to be larger to avoid link lose. It applies same for the _REG90_ where user can set the margin smaller initially and set them larger after the tone is found. 

CTCSS decoder has an optional high pass filter to filter out DC signal ( _REG92_ ), which can be used to get better RF frequency offset and low frequency noise immunity. 

The CTCSS can detect 120/180 degree phase shift ( _REG93_ ). 

CDCSS decoder uses the same setting as CDCSS encoder, and has also an optional DC block filter ( _REG94_ ). It can recognize the CDCSS code is exactly same or inversed ( _REG93_ ). 

#### **_1.5 SPI Interface_** 

The BK4815N has 3-wires SPI interferace. These 3 wires are SCK( _PIN25_ ), SCN( ̅̅̅̅̅̅̅ _PIN26_ ), SDATA( _PIN27_ ) for data exchange. SCK and SCN are input pins, ̅̅̅̅̅̅̅ while SDATA is bi-direction pin. 

BK4815N always latch data at the SCK rising edge and output its data at SCK falling edge. 

Proprietary and Confidential 

Page 15 of 47 

**BK4815N** 



<!-- Start of picture text -->
S\N<br>_>BEKEN<br>=a<br><!-- End of picture text -->



<!-- Start of picture text -->
Data[Addr] Data[Addr+1] Data[Addr+N]<br>0.5TCLK<br>SDATA Addr[6:0] + R/W D[15], D[14],  … , D[0] D[15:0] D[15:0]<br>SCK<br>SCN 0.5TCLK 0.5TCLK<br>Command<br><!-- End of picture text -->

**Figure 13. Three-wires Interface Timing** 



#### **_1.6 Power Management_** 

###### **Table 4. Power Control for Different Operational States** 

|**Blocks**|**Control Register**|**TX**|**RX**|**Idle**|
|---|---|---|---|---|
|DigitalSupply|REG12[2]|0|0|1|
|RC Calibration|REG12[3]|0|0|1|
|Central Bias|REG12[4]|0|0|1|
|BB DAC|REG12[5]|1|0|1|
|XTALOscillator|REG12[6]|0|0|1|
|SupplyforSynthesizer|REG12[7]|1|0|1|
|RFSynthesizer|REG12[8]|0|0|1|
|PA Driver|REG12[9]|0|1|1|
|TX Audio|REG12[10]|0|1|1|
|RX Audio|REG12[11]|1|0|1|
|RX ADC|REG12[12]|1|0|1|
|RX IF|REG12[13]|1|0|1|
|RX Front-end|REG12[14]|1|0|1|
|RX Bias|REG12[15]|1|0|1|
|DigitalState|REG112[14]|1|0|0|
|DigitalClock Enable|REG112[15]|1|1|0|
|Current||52 mA|60 mA|8 uA|



When switching from TX or RX mode to idle mode, register should be update in order of _REG12_ , and _REG112_ . Please refer to the application note for detailed operation. 

Proprietary and Confidential 

Page 16 of 47 

**BK4815N** 



### **2 Electrical Specifications** 

#### **_2.1 Absolute Maximum Ratings_** 

Maximum ratings are the extreme limits to which BK4815N can be exposed without permanently damaging it. Exposure to absolute maximum ratings for prolonged periods of time may affect BK4815N’s reliability. Table 5 specifies the absolute maximum ratings for BK4815N. 

**Table 5. Absolute Maximum Ratings** 

|**Parameter**|**Symbol**|**Min**|**Typ**|**Max**|**Unit**|
|---|---|---|---|---|---|
|SupplyVoltage|VDD|-0.3|—|+3.6|V|
|I/O pin voltage|VIO|-0.3|—|VDD+0.3|V|
|Storage Temperature|TS|–20|25|85|°C|



#### **_2.2 Recommended Operating Conditions_** 

The operating conditions are the physical parameters that BK4815N can operate within. The operating conditions for BK4815N are defined in Table 6. 

**Table 6. Recommended Operating Conditions** 

|**Parameter**|**Symbol**|**Min**|**Typ**|**Max**|**Unit**|
|---|---|---|---|---|---|
|SupplyVoltage|VDD|3.0|—|3.6|V|
|OperatingTemperature|TO|–10|25|60|°C|



###### **Notes:** 

All minimum and maximum specifications are guaranteed and apply across the recommended operating conditions. Typical values apply at VDD=3.3 V and 25 °C unless otherwise stated. Parameters are tested in production unless otherwise stated. For QFN4x4 24pin package, VDD range is 2.6-3.6V. 

The range of operating temperature mainly depends on the specification of the crystal. The frequency tolerance of the crystal should be within +/-2.5ppm during all operating conditions. 

#### **_2.3 Power Consumption Specification_** 

**Table 7. Power Consumption Specification** 

|**Parameter**|**Symbol**|**Test Condition**|**Min**|**Typ**|**Max**|**Unit**|
|---|---|---|---|---|---|---|
|Supply Current (RX Mode)|IRX|REG12=0x0603<br>REG112=0xA000|—|60|—|mA|
|Supply Current (TX Mode)|ITX|REG12=0xF823<br>REG112=0xE000|—|52|—|mA|
|Power Down Current|IPD|REG7=0xFFFF<br>REG112[15]=0|—|8|—|μA|



Proprietary and Confidential 

Page 17 of 47 

**BK4815N** 



#### **_2.4 Receiver Characteristics_** 

**Table 8. Receiver Characteristics** 

|Parameter|Symbol|Test Condition|Min|Typ|Max|Unit|
|---|---|---|---|---|---|---|
|||1|383|—|590||
|Operating Frequency|FOP|2<br>3|255<br>192|—<br>—|394<br>295|MHz|
|||4|128|—|197||
|Sensitivity|RXSENS|5,8,9|-124|-123|-122|dBm|
|Adjacent ChannelSelectivity|ACS|6, 8|63|64|65|dB|
|Blocking|BLK|7,8|81|82|84|dB|
|Inter-modulation|IMD|8|61|62|63|dB|
||**A**|**udio**|||||
|Earpiece output level|EARO|9||146||mVrms|
|SINAD|ASNR|9,10||37||dB|
|Amplitude response|ARES||-3||3|dB|
|Audio noise floor|ANF|||-81||dBm|
||**C**|**TCSS**|||||
|CTCSS sensitivity|CTSEN|||-121||dBm|
|CTCSS response time|CTRES||75||125|ms|
|Frequencyrange|SAF||62.5||250.3|Hz|
||<br>|**DCS**|||||
|CDCSS sensitivity|CDSEN|||-122|||
|CDCSS response time|CDRES|||171||ms|
|Code length|CLEN||23||24|Bit|
|Bit rate|BRATE|||134.4||Hz|
||**SE**|**LCALL**|||||
|SELCALL sensitivity|SELSEN|||-121||dBm|
|SELCALL response time|SELRES|||30||ms|
|Frequencyrange|IBSF||400||3000|Hz|
||**D**|**TMF**|||||
|DTMF sensitivity|DTSEN|||-122||dBm|
|DTMF response time|DTRES|||20||ms|
|High band frequencyrange|FH||1209||1633|Hz|
|Low band frequencyrange|FL||697||941|Hz|
||**FSK data mod**|**em(F3E and F2D)**|||||
|FSK sensitivity (1E-3 BER)|FSKSEN|||-122||dBm|
|FSK Best PER(32 bytespayload)|FSKBEST|||0.1|1|%|
|Over the air data rate|BAUD||1200||2400|bps|
|Test Condition:|||||||



1. Band 1 (REG4[8:7]=0x0) 

2. Band 2 (REG4[8:7]=0x2) 

3. Band 3 (REG4[8:7]=0x1) 

4. Band 3 (REG4[8:7]=0x3) 

5. 12 dB SINAD 

6. 1st adjacent channel (12.5kHz) 

7. Frequency offset > 1MHz 

8. According to ETSI standard (EN 300 296-1 V1.4.1) 

9. 1kHz tone, 1.5kHz deviation 

10. -50dBm input power 

Proprietary and Confidential 

Page 18 of 47 

**BK4815N** 



#### **_2.5 Transmitter Characteristics_** 

**Table 9. Transmitter Characteristics** 

|Parameter|Symbol|Test Condition|Min|Typ|Max|Unit|
|---|---|---|---|---|---|---|
|||1|383|—|590||
|Oti F|F|2|255|—|394|MH|
|perang requency|OP|3|192|—|295|z|
|||4|128|—|197||
|Output Power|POUT<br>|5|-20|0|5|dBm|
|Adjacent Channel Power Rejection|ACPR 1<sup>~~st~~</sup><br>||67|68|69|dBc|
|Alternate Channel Power Rejection|ACPR 2<sup>nd</sup>||70|72|73|dBc|
|Microphone Sensitivity|MICSENS|6||9||mV|
|SINAD|TSINAD|7||49||dB|
|Test Condition:|||||||



<mark>1. Band 1 (REG4[8:7]=0x0)</mark> 

<mark>2. Band 2 (REG4[8:7]=0x2)</mark> 

<mark>3. Band 3 (REG4[8:7]=0x1)</mark> 

<mark>4. Band 3 (REG4[8:7]=0x3)</mark> 

<mark>5. Depend on output matching and register settings</mark> 

<mark>6. 1.5kHz deviation</mark> 

<mark>7. At sensitivity level</mark> 

Proprietary and Confidential 

Page 19 of 47 

**BK4815N** 

<u>— ff</u> 

#### **_2.6 SPI Control Interface Characteristics_** 

**Table 10. SPI Control Interface Characteristics** 



<!-- Start of picture text -->
Test<br>Parameter  Symbol  Min  Typ  Max  Unit<br>Condition<br>PT SCLK Frequency fCLK 0 —  8 MHz<br>SCLK High Time tHIGH 25 —  —  ns<br>a es<br>a SCLK Low Time OO tLOW 25 —  —  ns<br>SDIO Input, SEN to SCLK ↑ Setup tS 20 —  —  ns<br>a es<br>a SDIO Input to SCLK ↑ Hold  tHSDIO 10 —  —  ns<br>a SEN Input to SCLK ↓ Hold  es tHSEN se 10 —  —  ns<br>PT SCLK ↑ to SDIO Output Valid  tCDV Read 2  —  25 ns<br>es SCLK ↑ to SDIO Output High Z  es tCDZ Read 2  —  25 ns<br>PT SCLK, SEN, SDIO, Rise/Fall Time  tR , tF —  —  10  ns<br>// //<br>SCLK<br>tR tF tHIGH tLOW tHSEN<br>tS<br>tHSDIO<br>SEN tS<br>//<br>SDIO A6 A5-A0 R/W D15 D14-D1 D0<br>//<br>Address In Data In<br>Figure 14. 3-Wire Control Interface Write Timing Diagram<br>//<br>SCLK<br>//<br>tHSEN<br>tS<br>tHSDIO tCDV<br>SEN tS tCDZ<br>// //<br>// //<br>SDIO A6 A5-A0 R/W D15 D14-D1 D0<br>// //<br>Address In ?TurnaroundCycle Bus Data Out<br><!-- End of picture text -->

**Figure 15. 3-Wire Control Interface Read Timing Diagram** 

Proprietary and Confidential 

Page 20 of 47 

**BK4815N** 



### **3 Register Definition** 

**Table 11. Interface Register Definition (Default setting for 446.00625 MHz, IF = 137 kHz)** 

|**Address**<br>（**DEC**）|**R/W**|**Default**<br>**Setting**|**Sub-**<br>**module**||**Description**|
|---|---|---|---|---|---|
|0|R|0x4815|Common|15:08|Device ID, read only|
|||||15|Channel spacing.<br>0: 12.5 kHz<br>1: 25 kHz|
|1|W/R|0x0000|Common|14<br>13|FSK_AIR  data rate.<br>0:1.2kbps<br>1:2.4kbps<br>FSK Mode<br>0: FSK<br>1: FSK AIR|
|||||12|DCS Mode<br>0: 23 bits<br>1: 24 bits|
|02 – 15||||RF/Ana|log|
|2|W|0x0019||||
|3|W|0x0027||||
|4|W|0xB641||08:07|VCO to LO divider number<br>0: 8; 1: 16; 2: 12; 3: 24|
|5|W|0xF770||||
|6|W|0xF274||||
|7|W|0x08F0||||
|8|W|0xFF33||||
|9|W|0xC3FA||||
|10|W|0xA2A3||||
|11|W|0x8800||12|PLL calibration 1->0->1|
|||0x0603(RX)<br>0xF823(TX)||15|RX central bias<br>1: Power down<br>0: Power up|
|12|W|<br>0xFFFF(Idle<br>)||14|RX front-end<br>1: Power down<br>0: Power up|



Proprietary and Confidential 

Page 21 of 47 

**BK4815N** 



|||||13|RX IF<br>1: Power down<br>0: Power up|
|---|---|---|---|---|---|
|||||12|RX ADC<br>1: Power down<br>0: Power up|
|||||11|RX Audio<br>1: Power down<br>0: Power up|
|||||10|TX Audio<br>1: Power down<br>0: Power up|
|||||09|TX PA<br>1: Power down<br>0: Power up|
|||||08|Synthesizer<br>1: Power down<br>0: Power up|
|||||07|LDO for synthesizer<br>1: Power down<br>0: Power up|
|||||06|XTAL oscillator<br>1: Power down<br>0: Power up|
|||||05|IF DAC<br>1: Power down<br>0: Power up|
|||||04|Central bias<br>1: Power down<br>0: Power up|
|||||03|RC calibration<br>1: Power down<br>0: Power up|
|||||02|Digital supply<br>1: Power down<br>0: Power up|
|13|W|0x03FD||00|IF Selection<br>0: 88kHz IF<br>1: 137kHz IF|
|14|W|0x5817||10|0: LO high injection<br>1: LO low injection|
|15|W|0x908B||||
|16|W|0x88F9||||
|17–63|||Digital R<br>|egisters|for TX Mode|
|17|W/R|0x5800|ADC<br>Filter|15:14|Filter gain (1, 1+1/4, 1+1/2, 2）|
|18|W/R|0x415C|AUDIO|06:02|Gain(-25~6 dB / 1dB)|
|19|W/R|0x08A0|AUDIO|13:00|Audio limiter value. -val ~ +val|



Proprietary and Confidential 

Page 22 of 47 

**BK4815N** 



|20|W/R|0x0000|AUDIO|15:13|Scrambling frequency<br>0: 2700Hz<br>1: 2800Hz<br>2: 2900 Hz<br>3: 3000Hz<br>4: 3100 Hz<br>5: 3200Hz<br>6: 3300 Hz<br>7: 3400Hz|
|---|---|---|---|---|---|
|||||15:08|Microphone signal level. (Read only)|
|21|W/R|0x002E|AUDIO|05:00|Real time microphone gain, same<br>unit as REG44[5:0]|
|||||15|Enable VOX (1)|
|22|W/R|0x3200|VOX|14|Speech detection speed<br>0: 1/512<br>1: 1/256|
|||||13:06|MIC_Level threshold for VoX|
|||||05:00|MIC Gain threshold for VoX|
|24|W/R|0x086C|DTMF|15:00|Low frequency. (freq/18466)*65536|
|25|W/R|0x13BA|DTMF|15:00|High frequency. (freq/18466)*65536|
|26|W/R|0x0000|DTMF|15:12|Twist (Unit: dB) 0~15dB. 1dB/step|
|||||15:08|Send address|
|28|W/R||FSK|<br>07:00|Size|
|30|W||FSK|15:00|Payload FIFO|
|||||15:13|FIFO Threshold|
||||||FIFO needs refilling (Read Only)|
|||||12|1: Needs refilling|
|31|W/R||FSK||0: No needs refilling|
|||||11|Clear TX FIFO (Write 1)|
|||||03:00|Total words in write FIFO (Read<br>Only)|
|||||15|Enable scrambling (1)|
|32|WR||FSK|14:08|Scrambling initial value<br>|
|||||00|FSK Tx Bit Select<br>1: From GPIO1 0: From inner frame|
|34|W/R||SELCA<br>LL|15:00|SELCALL tone Frequency<br>((Freq/18466)*65536)|
|||||14:13|The high 2 bits of CTCSS frequency|
|36|W/R||CTCSS|01:00|Phase change (Unit: Degree)<br>0: 0<br>1: 120<br>2: 180<br>3: 240|
|37|W/R||CTCSS|15:00|The low 16 bits of CTCSS frequency<br>((Freq/18466)*65536*4)|



Proprietary and Confidential 

Page 23 of 47 

**BK4815N** 



|38|W/R||CDCSS|11:00|High 12 bits of CDCSS if it has 24<br>bits or high 11 bits of CDCSS if it<br>has 23 bits|
|---|---|---|---|---|---|
|39|W/R||CDCSS|11:00|Low 12 bits of CDCSS|
|||||15|1: Enable in-band signal modulation<br>0: Disable in-band signal modulation<br>|
||||||In-band signal type|
|||||14:13|0: Audio<br>1: DTMF<br>2: FSK<br>3: SELCALL|
|40|W/R|0x8820|IB_MUX<br>SB_MU|12:09|In-band signal deviation<br>0.1x ~ 1.6x|
||||X|07|1: Enable sub-audio<br>0: Disable sub-audio|
|||||06|Sub-audio signal type<br>0: CTCSS<br>1: CDCSS|
|||||05:02|Sub-audio signal deviation<br>0.05x ~ 0.2x|
|41|W/R|0x2050|IB_MUX<br>TXUP|15:10|Deviation Tuning<br>0: 0.5x <br>1: 1x<br>2: 2x<br>63: 63x|
||||_|09:00|IF limiter. Unsigned. Real limiter<br>amplitude = 16*REG41<9:0>|
|42|W/R||RAMP|15:10<br>09:00|RAMP memory address<br>RAMP data|
|||||15|RAMP up enable<br>1:  Enable<br>0:  Disable|
|||||14|RAMP down enable<br>1:  Enable<br>0:  Disable|
|43|W/R|0x403F|RAMP|05:03|RAMP up speed<br>0: 0.08 ms<br>1: 0.08*2 ms<br>6: 0.08*64 ms 7: 0.08*128 ms|
|||||02:00|RAMP down speed, same as ramp<br>up speed definition|
|44|W/R|0x8A2E|AUD_A<br>GC|||
|45|W/R|0x1B80|AUD_A<br>|||
|46 – 63||Di|GC<br>gital Regist<br>|ers for Di|gital Walkie-Talkie|
|46|W/R|0x0072|CS_FLT<br>0_B2|Filter 0|Coefficence B2|
|47|W/R|0x00D3|CS_FLT<br>0_B3|Filter 0|Coefficence B3|
|48|W/R|0x6C0D|CS_FLT<br>0_A2|Filter 0|Coefficence A2|



Proprietary and Confidential 

Page 24 of 47 

**BK4815N** 



|49|W/R|0xD1D5|CS_FLT<br>0_A3|Filter 0|Coefficence A3|
|---|---|---|---|---|---|
|50|W/R|0xF717|CS_FLT<br>1_B2|Filter 1|Coefficence B2|
|51|W/R|0x0735|CS_FLT<br>1_B3|Filter 1|Coefficence B3|
|52|W/R|0x6D72|CS_FLT<br>1_A2|Filter 1|Coefficence A2|
|53|W/R|0xCD0B|CS_FLT<br>1_A3|Filter 1|Coefficence A3|
|54|W/R|0xDDAC|CS_FLT<br>2_B2|Filter 2|Coefficence B2|
|55|W/R|0x15D8|CS_FLT<br>2_B3|Filter 2|Coefficence B3|
|56|W/R|0x6F82|CS_FLT<br>2_A2|Filter 2|Coefficence A2|
|57|W/R|0xC723|CS_FLT<br>2_A3|Filter 2|Coefficence A3|
|58|W/R|0xC7A5|CS_FLT<br>3_B2|Filter 3|Coefficence B2|
|59|W/R|0x2206|CS_FLT<br>3_B3|Filter 3|Coefficence B3|
|60|W/R|0x721B|CS_FLT<br>3_A2|Filter 3|Coefficence A2|
|61|W/R|0xC235|CS_FLT<br>3_A3|Filter 3|Coefficence A3|
|||||15:8|DDS frequency|
|||||06|Digital walkie-talkie module input<br>clock enable|
|62|W/R|0x1141|CTRL|05:04|Digital walkie-talkie module output<br>config<br>0: 73.864kHz     1: 73.864kHz*2<br>2: 73.864kHz*4    1: 73.864kHz*8|
||||||Digital walkie-talkie module output<br>gain|
|||||02:00|0: 1x<br>1: 2x<br>2: 4x<br>3: 8x|
||||||4: 16x 5: 32x 6: 64x 7: 128x|
|64 – 111|||Digital R|egisters|for RX Mode|
|64|W/R|0x8000|RF_FLT|15:14<br>13:12|CIC gain<br>0: 1x<br>1: 1.25x<br>2: 1.5x<br>3: 2x<br>New HPF (RF_FLT) Control<br>0: Bypass<br>1: Run<br>2: Hold|



Proprietary and Confidential 

Page 25 of 47 

**BK4815N** 



|||||15:14|CS Filter Configuration:<br>0/1: Auto Selection|
|---|---|---|---|---|---|
|65|W/R|0xC000|MIXER||2: Broad Band     3: Narrow Band|
|||||12|Channel Selection result (read only)<br>0: Braod Band     1: Narrow|
|||||12|Parallel  Audio enable|
|||||11|Parallel  DTMF  enable|
|||||10|Parallel  FSK enable|
||||FM_DE|09|Parallel  SELCALL enable|
|66|W/R|0xD003|M|08|Enable CTCSS receive|
|||||07|Enable CDCSS receive|
|||||06|Impulse threshold<br>0: 0.5*pi<br>1:0.75*pi|
|67|W/R|0x2000|FM_DE<br>M|13<br>12:00|1: REG67<12:0> is averaged value<br>0: REG67<12:0> is real-time value<br>Ex-band noise|
||||RSSISN|15:14|SNR/RSSI/Exband_Noise update<br>period (samples)<br>0: 512; 1: 256, 2: 128, 3: 64|
|68|R||R|13:08|SNR indicator,  1 dB per step, 0 is<br>minimum|
|||||06:00|RSSI indicator,  1 dB per step, 0 is<br>minimum|
|||||15|1: Disable AFC<br>0: Enable AFC|
||||||AFC gain|
|69|W/R|0x67FF|AFC|14:13|0: 2x<br>1: 1x<br>2: 0.5x<br>3: 0.25x|
|||||12:00|AFC threshold (Unit: Hz)|
|||||15|AFC rail<br>1: AFC indicator > AFC threshold<br>0: AFC indicator < AFC threshold|
|70|R||AFC|14|Link state<br>1: Link is active<br>0: Link is lost|
|||||13:00|AFC residue frequency offset (Hz)|



Proprietary and Confidential 

Page 26 of 47 

**BK4815N** 



|||||13:08|SNR threshold for AFC|
|---|---|---|---|---|---|
||||||AFC Direction|
|71|W/R|0x0000|RSSI|07|1: Inversed mode<br>0: Normal mode|
|||||06:00|RSSI threshold for AFC|
|||||06:03|Scrambling frequency<br>0: 2700Hz<br>1: 2800Hz<br>2: 2900 Hz<br>3: 3000Hz|
|72|W/R|0xA002|BB_FLT||4: 3100 Hz<br>5: 3200Hz<br>6: 3300 Hz<br>7: 3400Hz|
|||||02:00|CTCSS_AMP<br>0: 8x<br>1: 4x<br>2: 2x<br>3: 8x|
|||||15|1: Hard Mute (Only affect when<br>REG73[10]=0)|
|||||14:07|RX audio level (Read only)|
|73|W/R|0x2600|AUDIO|03:00|Volume<br>0: -9dB<br>1: -8dB|
||||||2: -7dB ……<br>15: 6dB|
|75|W/R|0x7A86|DTMF|15:05|DTMF detection speed Suggest<br>value: 1960|
|76|W/R|0x6506|DTMF|||
|||||15:12|DTMF symbol address|
||||||DTMF Symbol frequency band|
|77|W||DTMF|11|1: High band<br>0: Low band|
|||||10:00|DTMF symbol low band or high<br>band tone frequency<br>=(Freq/4103)*4096|
|||||14|DTMF symbol loss indication|
|||||8|DTMF symbol found indication|
|78|W/R||DTMF|07:04|Address of found DTMF symbol|
|||||03:00|DTMF symbol numbers in search<br>table|
|||||15:08|Address|
|79|R||FSK|06:00|Type|
|81|W/R|0xB200|FSK|15:05|Symbol detection speed<br>Suggest value: 1424|



Proprietary and Confidential 

Page 27 of 47 

**BK4815N** 



|82|R||FSK|15:00|Payload FIFO|
|---|---|---|---|---|---|
|||||15:13|FIFO Threshold|
|||||12|FIFO needs reading out|
|||||11|Write 1 to clear RX FIFO|
|||||05|Free format type|
|83|W/R||FSK|04|CRCB_OK<br>1: No error in payload<br>0: Error in payload, will be<br>automatically updated at the end of<br>new RX packet|
|||||03:00|Total words in read FIFO|
|84|W/R|0xFC46|SELCA<br>LL|15:05|SELCALL detection speed<br>Suggest value: 2018|
|85|W/R|0x78A6|SELCA<br>LL|||
|||||15:12|SELCALL Symbol Address|
|86|W||SELCA<br>LL|11:00|SELCALL symbol frequency<br>Value = (Freq/18466)*4096)|
|||||14|HPF_BYPASS<br>1: Bypass<br>0: No bypass|
|||||13|1: Found symbol is lost|
|87|W/R|0x0000|SELCA<br>LL|12|SELCALL HPF coefficient selection<br>0: First group 1: Second group|
|||||07:04|The address of found SELCALL<br>symbol|
|||||03:00|SELCALL symbol numbers in<br>search table|
||||||CTCSS detection speed, for<br>example:|
|89|W/R|0xF7A0|CTCSS|15:04|0xFF8 for high sensitivity and >1<br>second response time<br>0xF7A for normal sensitivity and<br><100 ms response time|
|90|W/R|0x3C46|CTCSS|||
|||||15:13|CTCSSS symbol address|
|91|W||CTCSS|12:00|CTCSSS symbol frequency<br>Value = (Freq/18466)*65536*8|
|92|W/R|0x0000|CTCSS|14|1: Bypass HPF|



Proprietary and Confidential 

Page 28 of 47 

**BK4815N** 



|||||13|1: Found CTCSS symbol is lost|
|---|---|---|---|---|---|
|||||11:08|High 4bits address of found CTCSS<br>(for standard mode)|
|||||07:04|The address of found CTCSS<br>symbol|
|||||02:00|CTCSS symbol numbers in search<br>table|
|93|W/R|0x0000|CTCSS|08:02|Loss threshold(Time duration for<br>loss report)<br>15: 11ms<br>31: 22ms<br>63: 55ms<br>127: 110ms|
|||||01:00|CTCSS detect phase change<br>0: 0<br>1:120240<br>2:180<br>3: Reserved|
|||||15<br>14:08|1: Bypass HPF<br>Address of found CDCSS|
|94|W/R|0x00E8|CDCSS|07:06|Synchronization Threshold<br>0: 32<br>1: 64<br>2: 128<br>3: 256|
|||||02|Phase of found CDCSS<br>0: Normal<br>1: Inverse|
|||||1|1: CDCSS loss|
|95|W/R||CDCSS|11:00|High 12 bits of CDCSS if it has 24<br>bits or high 11 bits of CDCSS if it<br>has 23 bits|
|96|W/R||CDCSS|11:00|Low 12 bits of CDCSS|
|||||15:08|Number of CDCSS code to be<br>searched|
|97|W/R||CDCSS|06:00|Address of RX CDCSS (Should be<br>wrote before REG95/96)|
||||||DCS code match threshold (Last for<br>N times)|
|98|W/R|0x0005|CDCSS|03:01|0: 1 time<br>1: 2 times<br>2: 3 times…… 7: 8 times|
|||||00|Slice calculation enable|
|99|W/R|0x0000|FSK_AI<br>R|15<br>14|HPF bypass<br>Inverse the FSK AIR output|



Proprietary and Confidential 

Page 29 of 47 

**BK4815N** 



|||||11:06|Coefficient A1|
|---|---|---|---|---|---|
|||||05:00|Coefficient A2|
|100|W/R|0x0000|Receive<br>r|07|0: Bypass new RSSI2 path<br>1: Run new RSSI2 path|
|||||06:00|RSSI2, 1 dB/Step, 0 is maximum|
|101|R||RF<br>AGC|15:08<br>07:00|AGC_H; IF RSSI high level indicator<br>AGC_L; IF RSSI low level indicator|
|102|W/R|0x0000|Ex-band<br>Noise|13|Link-Loss judgment selection :<br>1 : Ex-Noise<br>0: Rssi_SNR|
|||||12:00|Baseband out-of-band noise level|
|||||15|RX AGC<br>0: Enable<br>1: Disable|
|||||14:08|RSSI Offset|
|103|W/R|0x431F|RF<br>AGC|04|RF LNA gain selection<br>0: Low gain<br>1: High gain|
|||||02:00|IF PGA gain selection<br>0: 0dB<br>1: 3dB<br>2: 6dB……<br>7: 21dB|
|104|W/R|0x0000|RF<br>AGC|||
|||||15|1: Up enable|
|105|W/R|0x0000|RX<br>Audio|14|1: Down enable|
|||||09:00|End value (0~1023)|
|106|W/R|0x0000|RX<br>Audio|15:09|Clock divide ratio DIV<br>0: 1<br>1: 2<br>2: 3……<br>127: 128|
|||||08:00|Start value (0~511)|
|107-11||||RF / An|alog|
|107|W/R|0x3415|TX<br>Audio|||
|108|W/R|0x6927|RX<br>Audio|14:11|PGA gain<br>0: -24dB W/R 1: -22dB<br>2: -20dB…… 15: +6dB|
|109|W/R|0x4600(RX)<br>0x4700(TX)|Analog|15:13<br>12|PA output power control<br>-20dBm~+6dBm(not linear)<br>PA ramp up/down enable|



Proprietary and Confidential 

Page 30 of 47 

**BK4815N** 



|||||09|Synthesizer state control<br>1: By SPI<br>0: By DSP|
|---|---|---|---|---|---|
||||||Synthesizer state|
|||||08|1: TX mode<br>0: RX mode|
|112-119|||Con|trolandI|nterrupt|
|||||15|1: Power up digital section<br>0: Power down digital section|
|112|W/R|0xA000(RX)<br>0xE000(TX)|Operati<br>on<br>|14|Digital logic operation mode<br>1: TX mode<br>0: RX mode|
||||Control|13|1: Normal work,<br>0: Software reset, it will not reset<br>registers|
|113|W/R||Frequen<br>cy<br>control|15:00|High 16 bits of First Channel<br>Frequency|
|114|W/R||Frequen<br>cy<br>control|15:00|Low 16 bits of First Channel<br>Frequency|
|||||15|1: Enable interrupt<br>0: Disable all interrupt|
|||||14|Enable FSK transmit success<br>interrupt|
|||||13|Enable FSK transmitter FIFO need<br>fill interrupt|
|||||12|Enable FSK head receive success<br>interrupt|
|||||11|Enable FSK receiver FIFO need<br>read interrupt|
|115|W/R||Interrupt<br>MASK|10|Enable DTMF receive interrupt|
|||||9|Enable SELCALL receive interrupt|
|||||8|Enable CTCSS receive interrupt|
|||||7|Enable CTCSS tone loss interrupt|
|||||6|Enable link lost interrupt|
|||||5|Enable CDCSS receive interrupt|
|||||4|Enable CDCSS lost  interrupt|
|||||3|Enable VOX interrupt|
|||||1|Enable FSK receive complete<br>interrupt|
|116|W/R||Interrupt|15|1: Reset all flags to zeros|



Proprietary and Confidential 

Page 31 of 47 

**BK4815N** 

<u>— 2</u> 

|||flags<br>|14|FSK transmit success interrupt|
|---|---|---|---|---|
|||(Active<br>high)<br>Set by|13|FSK transmitter FIFO need fill<br>interrupt|
|||hardwar<br>e and|12|FSK head receive success interrupt|
|||<br>reset by<br>softwar|11|FSK receiver FIFO need read<br>interrupt|
|||e (write<br>1 will|10|DTMF receive interrupt|
|||<br>clear|9|SELCALL receive interrupt|
|||interrupt<br>bit to 0)|8|CTCSS receive interrupt|
||||7|CTCSS loss interrupt|
||||6|Link lose interrupt|
||||5|CDCSS receive interrupt|
||||4|CDCSS lost match interrupt|
||||3|VOX voice detected interrupt|
||||1|FSK receive complete interrupt|
||||11|1: Bypass gated clock|
||||10|0: Normal IF, low side injection<br>1: Inverse IF, high side injection|
||||09|1: Send CTCSS / CDCSS / DTMF /<br>SELCALL / FSK or TX Audio to RX<br>Audio|
|||||Select the signal to RX Audio<br>0: CDCSS<br>1: CTCSS|
|||||2: SELCALL|
|117<br>W/R|0x0400|GPIO|08:06|3: DTMF<br>4: FSK|
|||||5: Audio<br>6: FSK AIR|
|||||7: Force to ―0‖|
||||05:03|GPIO1 mode control<br>0: Normal input<br>1: Output low<br>2: Output high<br>Other: Invalid|



Proprietary and Confidential 

Page 32 of 47 

**BK4815N** 





<!-- Start of picture text -->
GPIO1 mode control<br>0: Normal input<br>1: Output low<br>02:00  2: Output high<br>3: Output CTCSS signal<br>4: Output CDCSS signal<br>Other: Invalid<br>1  GPIO1 input value<br>118  R  GPIO<br>0  GPIO0 input value<br>120-127  Enhanced Functions<br>FSK bit rate setting  =<br>reference_clock/bit_rate<br>122  W/R  0x46A3  FSK<br>Default is: 26e6/1200<br>MSB 2 bits of CDCSS bit rate setting =<br>reference_clock/bit_rate<br>123  W/R  0x0002  CDCSS<br>Default is: 26e6/134.4<br>LSB 16 bits of CDCSS bit rate setting =<br>reference_clock/bit_rate<br>124  W/R  0xF826  CDCSS<br>Default is: 26e6/134.4<br>IF DDS Setting, unsigned<br>125  W/R  0xED69  MIXER  DDS_IF = REG125*2 = IF * 2^18 /<br>(26MHz/88kHz)<br>Frequen<br>126  W/R  0xFFF5  cy  RX IF frequency, signed, high 16 bits<br>control<br>Frequen<br>127  W/R  0x3568  cy  RX IF frequency, signed, low 16 bits<br>control<br><!-- End of picture text -->

Proprietary and Confidential 

Page 33 of 47 

**BK4815N** 

<u>— ff</u> 

### **4 Pin Assignment** 



<!-- Start of picture text -->
32 31 30 29 28 27 26 25<br>IFQN 1 24 GNDDIG<br>IFQP 2 23 VBATD<br>VCCXTAL 3 22 CDVDD<br>XO 4 BK4815N 21 VCCVCO<br>QFN4x4<br>XI 5 20 NC<br>VDDXTAL 6 19 NC<br>VCCCB 7 18 VRAMP<br>EARO 8 17 PAOUT<br>9 10 11 12 13 14 15 16<br>IFIN IFIP INTN GPIO1 GPIO0 SDATA SCN SCK<br>NC NC VCCPLL VCCIF MICN MICP LNAIN VCCRF<br><!-- End of picture text -->

**Figure 16. BK4815N Pin Assignment (Top View)** 

**Table 12. BK4815N 4mmx4mm 32-Pin Definition** 

|**Pin#**<br>~~|~~|**Name**<br>|**Direction **<br>~~|S~~|**Function **<br>~~|~~|
|---|---|---|---|
|1<br><br><br>|IFQN<br><br><br>|Output<br> <br><br>|Baseband analog ouput signalQN<br> <br>~~a~~|
|2<br><br>|IFQP<br><br>|Output<br><br>|Baseband analog ouput signalQP|
|3<br><br>|VCCXTAL<br><br>|Input<br><br>|Powersupply, 3.0Vto 3.6V|
|4<br><br>|XO<br><br>|Input<br><br>|Crystaloscillatorport, output<br>|
|5<br>~~(~~<br>|XI<br>~~a~~<br>|Output<br>~~W~~<br>|Crystaloscillatorport,input<br>~~Se~~<br>|
|6<br><br><br>|VDDXTAL<br>  <br><br>|Output<br><br><br>|XTALOSCregulatoroutput<br><br>~~a~~|
|7<br><br>|VCCCB<br><br>|Input<br><br><br>|Powersupply, 3.0Vto 3.6V|
|8<br><br>|EARO<br><br>|Output<br><br>~~a~~|Earpiece output|
|9<br><br>|NC<br>~~a~~<br>|||
|10<br><br>|NC<br>~~a~~<br>|~~a~~||
|11<br><br>|VCCPLL<br>~~a~~<br>|Input<br>|Powersupply, 3.0Vto 3.6V|
|12<br><br>|VCCIF<br>~~a~~<br>|Input<br>~~a~~|Powersupply, 3.0Vto 3.6V|
|13<br><br>|MICN<br>~~a~~<br>|Input<br>|Microphoneinput,negative|
|14<br><br>|MICP<br>~~a~~<br>|Input<br>~~a~~|Microphone output, positive|
|15<br><br>|LNAIN<br>~~a~~<br>|Input<br>|Input of low noise amplifier|
|16<br><br>|VCCRF<br>~~a~~<br>|Input<br>~~a~~|Powersupply, 3.0Vto 3.6V|
|17<br><br>|PAOUT<br>~~a~~<br>|Output<br>|Output ofpoweramplifier|
|18<br><br>|VRAMP<br>~~a~~<br>|Output<br>~~a~~|PA regulatoroutput|
|19<br><br>|NC<br>~~a~~<br>|||
|20<br>|NC<br>~~a~~|~~a~~||
|21<br>|VCCVCO<br>~~a~~|Input|PowerSupply, 3.0V~3.6V|
|22<br><br>|CDVDD<br>~~a~~|Output<br>~~a~~|Digital regulatoroutput|
|23<br>~~a~~|VBATD|Input|Power Supply,3.0 V~ 3.6 V|



Proprietary and Confidential 

Page 34 of 47 

**BK4815N** 

<u>— 2</u> 

|24|GNDDIG|Input|Ground|
|---|---|---|---|
|25|SCK|Input|SPIclock|
|26|SCN|Input|SPIenable|
|27|SDATA|I/O|SPIdata|
|28|GPIO0|I/O|Generalpurposeinput/output.Canbe used asDCS output|
|29|GPIO1|I/O|Generalpurposeinput/output|
|30|INTN|Output|Interrupt|
|31|IFIP|Output|Baseband analog ouput signal IP|
|32|IFIN|Output|Baseband analogouput signal IN|





Proprietary and Confidential 

Page 35 of 47 

**BK4815N** 



### **5 Typical Application Schematic** 



<!-- Start of picture text -->
VDD<br>NOVQ0Y<br>C1<br>6.8 pF<br>C2<br>1 24<br>IFQN GNDDIG<br>2 23 C14<br>IFQP VBATD 1 uF<br>3 22<br>4 C3 Crystal DS VCCXTAL CDVDD<br>6.8 pF 4 XO U1 VCCVCO 21<br>5 20<br>XI BK4815N NC<br>C4 6 19 C13<br>1 uF 7 VDDXTAL NC 18 1 uF<br>VCCCB VRAMP<br>EARO C5 8 EARO PAOUT el 17<br>1 uF<br>o hn a =<br>S e a<br>C6 C7 L1<br>1 nF 1 uF 27 nH<br>C12<br>100 pF<br>. T J (i) —t><br>C8 C10<br>1 uF 4.7 pF C11<br>10 uF<br>MICIN RX  TX<br>a] port 7 port<br>Figure 17. BK4815N Application<br>INTN GPIO1 GPIO0 SDATA SCN SCK<br>32 31 30 29 28 27 26 25<br>IFIN IFIP INTN GPIO1 GPIO0 SDATA SCN SCK<br>NC NC VCCPLL VCCIF MICN MICP LNAIN VCCRF<br>9 10 11 12 13 14 15 16<br>C9 1 uF<br><!-- End of picture text -->

Proprietary and Confidential 

Page 36 of 47 

**BK4815N** 

<u>— ff</u> 

### **6 Package Information** 

QFN 4mmx4mm 32pin package is available for BK4815N. Detail information of the package is shown below: 



<!-- Start of picture text -->
-#——— ) ———_+ 4k 2<br>*\_LASER_PIN MARKT 1D. gUpuuouuul|D H qeej)?<br>5 E<br>D Rae<br>0 D g q<br>of 02 q<br>>) | q<br>=) q<br>nanoananhn<br>“s TOP MEW b [10.07 @)<br>Gobeonngedfo SIDE VIEW '{ZS]0.08) BOTTOM ViEW<br>=<br><2 ay<br>iis Le TT><br>DETAIL A<br>Figure 18. QFN 4x4 32 Pin Package diagram<br><!-- End of picture text -->



Proprietary and Confidential 

Page 37 of 47 

**BK4815N** 



**Table 13. QFN 4x4 32 Pin Package dimensions** 

|**Parameter**|**Min**|**Typ**|**Max**|**Unit**|
|---|---|---|---|---|
|A|0.80|0.85|0.90|mm|
|A1|0.00|0.02|0.05|mm|
|A2|0.60|0.65|0.70|mm|
|A3||0.20 REF||mm|
|b|0.15|0.20|0.25|mm|
|D|3.90|4.00|4.10|mm|
|E|3.90|4.00|4.10|mm|
|D2|2.80|2.90|3.00|mm|
|E2|2.80|2.90|3.00|mm|
|e|0.30|0.40|0.50|mm|
|H||0.30 REF||mm|
|K||0.25 REF||mm|
|L|0.25|0.30|0.35|mm|
|R|0.09|||mm|
|c1||0.10||mm|
|c2||0.10||mm|





##### **Soldering layer content** 

|**Content**|**width **|**unit**|
|---|---|---|
|Ni|0.5-2.0|um|
|Pd|0.02-0.15|um|
|Au|0.003-0.015|um|



##### **Storage Caution** 

1. Calculated shelf life in vacuum sealed bag 12 months at<40 ℃ and 90% relative humidity (RH). 

2. Peak package body temperature 260 ℃ . 

3. After vacuum sealed bag is opened ,devices that will be subjected to reflow solder or other high temperature process must a) Mounted within 168 hours of factory conditions<40 ℃ /60%. 

   - b) Stored at 10% RH. 

Proprietary and Confidential 

Page 38 of 47 

**BK4815N** 



<!-- Start of picture text -->
a \_<br>>BEKEN<br>=a<br><!-- End of picture text -->

##### **Package Markings(Top Marks)** QFN 32 



<!-- Start of picture text -->
BK4815<br>NVYWWLA<br>N: Character N<br>V: Version Number<br>YWW: Y=Year,WW=Workweek<br>L: The last nubmer of Wafer<br>A: Assembly house number<br><!-- End of picture text -->



Proprietary and Confidential 

Page 39 of 47 

**BK4815N** 

<u>— ff</u> 

### **7 Tape and Reel Information** 

All dimensions are in millimeters by default. 

#### **_7.1 Tape Information_** 

The figure below shows the dimensions of the tape for the BK4815N QFN. 



<!-- Start of picture text -->
9154100<br>2.00 £.05 SEE MOTE 3 8.00 61.50 MIN<br>DEG 4.00 SE MOE | 154.0<br>P03 MAK -H- 00/0x! 0/2L 00000,0_ 0 5.50 4.05<br>: r \] >| js yo{ je] ho SEE NITE 3<br>L. « Ly \ as toy poni<br>to CRE ba<br>‘)<br>N}<br>SILER<br>SECTIN<br>A - 4<br><!-- End of picture text -->

**Figure 19. Tape Dimensions** 

|**A0**|**B0**|**K0**|**Unit**|**Notes**|
|---|---|---|---|---|
|4.35|4.35|1.1|mm|1. 10 sprocket hole pitch cumulative<br>tolerance ±0.2.<br>2. Camber in compliance with EIA 481.<br>3. Pocket position relative to sprocket<br>hole measured as true position of<br>pocket,not pockethole.|



Proprietary and Confidential 

Page 40 of 47 

**BK4815N** 



<!-- Start of picture text -->
=e<br>_> BEKEN<br>=i<br><!-- End of picture text -->

#### **_7.2 Cover Information_** 



<!-- Start of picture text -->
COVER TAPE WIDTH "Wr<br>9.200.10mm<br>76.2(3.0 INCHES)NOMINAL INSIDE<br>CORE DIAMETER<br>ULES<br>NWN<br>0.048:40.005 W{igG RM<br>OY T QM<br>— Mili AAA)<br>fatty | vvvunatath<br>flfiltisttiKathatinif{-—- ' 4} viii}ini<br>Ahan) i iit]<br>Why | MILL<br>WOO Lyi)<br>WNOSL)SELLE“.' tyUII<br>HEAT SEALABLE SSS<br>ADHESIVE THIS SIDE SSSI SEE<br><!-- End of picture text -->

**Figure 20. Cover Dimensions** 

###### **Note:** 

1. Reel to contain 300 meters of splice free material. 

2. Material: Polyester film with antistatic coating and adhesive coating. 

3. Color: Transparent, natural 



Proprietary and Confidential 

Page 41 of 47 

**BK4815N** 



<!-- Start of picture text -->
Ss NY<br>“> BEKEN<br>=i<br><!-- End of picture text -->

#### **_7.3 Reel Information_** 



<!-- Start of picture text -->
>|<br>|<br>[s =~ A \ ; oo<br>PQese-| I ce.<br>\\\\ NaNae ‘A Hi|| | AycMAY 130%<br>\ Hi \\ / “DTREF 2.0405<br>|=\<br>Detail"s* > ae Jk<br>‘ (MEASURED AT HUB) | wi<br>NN<br>Zo (MEASURED AT HUB) | [we<br>PS<br><!-- End of picture text -->

**Figure 21. Reel Dimensions** 



<!-- Start of picture text -->
+0.6mm<br>Nominal Hub Width  W1  W2 MAX  a  b  Unit<br>-0.4mm<br>12  12.8 18.2  1.5 96.5 mm<br><!-- End of picture text -->



Proprietary and Confidential 

Page 42 of 47 

**BK4815N** 



### **8 Solder Reflow Profile** 

|Temperature<br>TL<br>TP|Ts(min)<br>Ts(max)<br>TP(min)<br>TP(max)<br>~~tS~~<br>Preheat<br>~~t~~L<br>tP<br>Ramp Up<br>Ramp Down<br>Critical Zone<br>tLto tP|
|---|---|
||Time<br>~~t 25C to Peak~~<br>**Figure 22. Classification Reflow Profile**|
|ProfileFe|ature<br>Specification|
|Average|Ramp-UpRate (tsmaxto tp)<br>3°C/secondmax.<br> <br>|
||<br>Temperature Min(Tsmin)<br>150°C|
|Pre_heat|<br>TemperatureMax(Tsmax)<br>200°C<br><br>|
||Time (ts)<br>60-180 seconds|
|Time Mai|ntained<br>Temperature (TL)<br>217°C|
|above|Time (tL)<br>60-150 seconds|
|Peak/Cla|ssification Temperature (Tp)<br>260°C|
|Time with|in 5°C of Actual Peak Temperature (tp)<br>20-40 seconds|
|Ramp-Do|wn Rate 6<br>6°C/secondmax.|
|Time 25°|C to Peak Temperature 8<br>8 minutes max.|



##### **RoHS Compliant** 

The product does not contain lead, mercury, cadmium, hexavalent chromium, PBB&PBDE content in accordance with directive 2002/95/EC(RoHS). 

##### **ESD Sensitivity** 

Integrated circuits are ESD sensitive and can be damaged by static electricity. Proper ESD techniques should be used when handling these devices. 



Proprietary and Confidential 

Page 43 of 47 

**BK4815N** 

<u>— 2</u> 

### **9 Order Information** 

|**Part number**|**Package**|**Packing**|**MOQ (ea)**|
|---|---|---|---|
|BK4815NQB|QFN|Tape Reel|3 k|



Remark: 

MOQ: Minimum Order Quantity 



Proprietary and Confidential 

Page 44 of 47 

**BK4815N** 



### **10 Additional Reference Resources** 

- Application Notes 

- Register Table 

- Schematic & Layout 

- Frequency Calculator 



Proprietary and Confidential 

Page 45 of 47 

**BK4815N** 

<u>— 2</u> 

### **11 Revision History** 

|**Version**||**Change Summary**|**Date**|**Author**|
|---|---|---|---|---|
|Rev.1.0|Initial Release||2016/09/01|PH|





Proprietary and Confidential 

Page 46 of 47 

**BK4815N** 



### **12 Contact Information** 

Beken Corporation Technical Support Center 

Website: www.bekencorp.com Email: info@bekencorp.com 

Shanghai Headquarters: Building 41, Capital of Tech Leaders, 1387 Zhangdong Road, Zhangjiang High-Tech Park, Pudong New District, Shanghai, China Phone: 86-21-51086811, 60871276 Fax: 86-21-60871277 Postal Code: 201203 

Shenzhen Branch: Room 310, HKUST SZ IER Building, No. 9 Yuexing 1st Rd., Nanshan Hi-Tech Park, Shenzhen, China Phone: 86-755-2655 1063 Postal Code: 518057 Taipei Branch: 2F.-1, No.49, Ln.35, Jihu Rd., Neihu Dist., Taipei City, Taiwan Phone: 886-2-26275780 



<!-- Start of picture text -->
Phone:   886-2-26275780<br><!-- End of picture text -->

Proprietary and Confidential 

Page 47 of 47 

