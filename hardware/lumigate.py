#!/usr/bin/env python
"""LumiGate v3 — standalone ESP32-S3 Art-Net/sACN -> isolated DMX gateway (SKiDL).

Self-contained board (no plug-in module):
  ESP32-S3-WROOM-1-N8  +  W5500 SPI-Ethernet + HR911105A MagJack
  CH340K USB-UART (auto-reset) on a USB-C (data) inlet  +  SY8089 5V->3.3V buck
  Isolated DMX out: CA-IS3082W + B0505S iso-DC-DC + XLR-3 (+ SM712 TVS, 120R term)
  5 status LEDs (red/green/yellow/blue/white) direct on GPIOs

Pin map (ESP32-S3): SPI SCLK=IO12 MOSI=IO11 MISO=IO13 CS=IO10 INT=IO14 ETHRST=IO9;
  DMX DI=IO17 RO=IO18 DE=IO8; LEDs R=IO1 G=IO2 Y=IO6 B=IO7 W=IO15; UART0 TX=IO43 RX=IO44.

NB: the W5500<->MagJack analog section (TX/RX pairs, center-tap/TOCAP) follows the WIZnet
reference; verify against the W5500 datasheet + HR911105A before fabricating.

Regenerate:  python lumigate.py   -> lumigate.net
"""
import os
from skidl import Part, Pin, Net, TEMPLATE, SKIDL, generate_netlist, ERC, set_default_tool, KICAD9

set_default_tool(KICAD9)
PT = Pin.types
HERE = os.path.dirname(os.path.abspath(__file__))


def mk(name, prefix, fp, pins, value=None):
    t = Part(tool=SKIDL, name=name, ref_prefix=prefix, dest=TEMPLATE)
    for num, pname, func in pins:
        t += Pin(num=str(num), name=pname, func=func)
    t.footprint = fp
    if value:
        t.value = value
    return t


def two(name, prefix, fp, value):
    return mk(name, prefix, fp, [(1, '1', PT.PASSIVE), (2, '2', PT.PASSIVE)], value)()


def R(ref, val, fp='Resistor_SMD:R_0402_1005Metric'):
    r = two('R', 'R', fp, val); r.ref = ref; return r


def C(ref, val, fp='Capacitor_SMD:C_0402_1005Metric'):
    c = two('C', 'C', fp, val); c.ref = ref; return c


# ---- Nets -------------------------------------------------------------------
P5V = Net('+5V'); P3V3 = Net('+3V3'); GND = Net('GND')
VISO = Net('VISO'); GND2 = Net('GNDISO')                       # isolated DMX bus side
USB_DP = Net('USB_DP'); USB_DM = Net('USB_DM'); CC1 = Net('CC1'); CC2 = Net('CC2')
S3_TX = Net('S3_TX'); S3_RX = Net('S3_RX')                     # UART0 console/flash
EN = Net('EN'); IO0 = Net('IO0'); DTR = Net('DTR'); RTS = Net('RTS')
SCLK = Net('SCLK'); MOSI = Net('MOSI'); MISO = Net('MISO')
ETH_CS = Net('ETH_CS'); ETH_INT = Net('ETH_INT'); ETH_RST = Net('ETH_RST')
DMX_TX = Net('DMX_TX'); DMX_RX = Net('DMX_RX'); DMX_EN = Net('DMX_EN')
DMX_A = Net('DMX_A'); DMX_B = Net('DMX_B')
LED_R = Net('LED_R'); LED_G = Net('LED_G'); LED_Y = Net('LED_Y'); LED_B = Net('LED_B'); LED_W = Net('LED_W')
# W5500 analog / Ethernet
XI = Net('XI'); XO = Net('XO'); V12 = Net('W5500_1V2'); TOCAP = Net('TOCAP')
ETH_TXP = Net('ETH_TXP'); ETH_TXN = Net('ETH_TXN'); ETH_RXP = Net('ETH_RXP'); ETH_RXN = Net('ETH_RXN')
ETH_TCT = Net('ETH_TCT'); ETH_RCT = Net('ETH_RCT'); ETH_LL = Net('ETH_LINKLED'); ETH_AL = Net('ETH_ACTLED')
LX = Net('BUCK_LX'); FB = Net('BUCK_FB')

# ============================================================================
# U1  ESP32-S3-WROOM-1-N8  (module)
# ============================================================================
S3 = mk('ESP32-S3-WROOM-1-N8', 'U', 'RF_Module:ESP32-S3-WROOM-1',
        [(1, 'GND', PT.PWRIN), (2, '3V3', PT.PWRIN), (3, 'EN', PT.INPUT),
         (4, 'IO4', PT.BIDIR), (5, 'IO5', PT.BIDIR), (6, 'IO6', PT.BIDIR), (7, 'IO7', PT.BIDIR),
         (8, 'IO15', PT.BIDIR), (9, 'IO16', PT.BIDIR), (10, 'IO17', PT.BIDIR), (11, 'IO18', PT.BIDIR),
         (12, 'IO8', PT.BIDIR), (13, 'IO19', PT.BIDIR), (14, 'IO20', PT.BIDIR), (15, 'IO3', PT.BIDIR),
         (16, 'IO46', PT.BIDIR), (17, 'IO9', PT.BIDIR), (18, 'IO10', PT.BIDIR), (19, 'IO11', PT.BIDIR),
         (20, 'IO12', PT.BIDIR), (21, 'IO13', PT.BIDIR), (22, 'IO14', PT.BIDIR), (23, 'IO21', PT.BIDIR),
         (24, 'IO47', PT.BIDIR), (25, 'IO48', PT.BIDIR), (26, 'IO45', PT.BIDIR), (27, 'IO0', PT.BIDIR),
         (28, 'IO35', PT.BIDIR), (29, 'IO36', PT.BIDIR), (30, 'IO37', PT.BIDIR), (31, 'IO38', PT.BIDIR),
         (32, 'IO39', PT.BIDIR), (33, 'IO40', PT.BIDIR), (34, 'IO41', PT.BIDIR), (35, 'IO42', PT.BIDIR),
         (36, 'RXD0', PT.BIDIR), (37, 'TXD0', PT.BIDIR), (38, 'IO2', PT.BIDIR), (39, 'IO1', PT.BIDIR),
         (40, 'GND', PT.PWRIN), (41, 'EP', PT.PWRIN)], value='ESP32-S3-WROOM-1-N8')
U1 = S3(); U1.ref = 'U1'
U1['GND'] += GND; U1[40] += GND; U1['EP'] += GND
U1['3V3'] += P3V3
U1['EN'] += EN
U1['IO0'] += IO0
U1['TXD0'] += S3_TX; U1['RXD0'] += S3_RX
# SPI -> W5500
U1['IO12'] += SCLK; U1['IO11'] += MOSI; U1['IO13'] += MISO
U1['IO10'] += ETH_CS; U1['IO14'] += ETH_INT; U1['IO9'] += ETH_RST
# DMX
U1['IO17'] += DMX_TX; U1['IO18'] += DMX_RX; U1['IO8'] += DMX_EN
# LEDs
U1['IO1'] += LED_R; U1['IO2'] += LED_G; U1['IO6'] += LED_Y; U1['IO7'] += LED_B; U1['IO15'] += LED_W
# module decoupling
Cs1 = C('C1', '100nF'); Cs1[1] += P3V3; Cs1[2] += GND
Cs2 = C('C2', '10uF', 'Capacitor_SMD:C_0805_2012Metric'); Cs2[1] += P3V3; Cs2[2] += GND
# EN reset RC + BOOT/EN pull-ups
Ren = R('R1', '10k'); Ren[1] += P3V3; Ren[2] += EN
Cen = C('C3', '1uF'); Cen[1] += EN; Cen[2] += GND   # EN power-on RC: 10k x 1uF = 10ms ramp (Espressif ref; was 100nF=1ms, marginal)
Rb = R('R2', '10k'); Rb[1] += P3V3; Rb[2] += IO0
SWb = mk('SW_PUSH', 'SW', 'Button_Switch_SMD:SW_SPST_B3U-1000P',
         [(1, '1', PT.PASSIVE), (2, '2', PT.PASSIVE)], value='BOOT')
SW1 = SWb(); SW1.ref = 'SW1'; SW1[1] += IO0; SW1[2] += GND
SW2 = SWb(); SW2.ref = 'SW2'; SW2.value = 'RST'; SW2[1] += EN; SW2[2] += GND

# ============================================================================
# U2  W5500 SPI Ethernet  + Y1 25MHz + magjack
# ============================================================================
W5 = mk('W5500', 'U', 'C32843:LQFP-48_L7.0-W7.0-P0.50-LS9.0-BL',
        [(1, 'TXN', PT.PASSIVE), (2, 'TXP', PT.PASSIVE), (3, 'AGND', PT.PWRIN), (4, 'AVDD', PT.PWRIN),
         (5, 'RXN', PT.PASSIVE), (6, 'RXP', PT.PASSIVE), (7, 'DNC', PT.NOCONNECT), (8, 'AVDD', PT.PWRIN),
         (9, 'AGND', PT.PWRIN), (10, 'EXRES1', PT.PASSIVE), (11, 'AVDD', PT.PWRIN), (12, 'NC', PT.NOCONNECT),
         (13, 'NC', PT.NOCONNECT), (14, 'AGND', PT.PWRIN), (15, 'AVDD', PT.PWRIN), (16, 'AGND', PT.PWRIN),
         (17, 'AVDD', PT.PWRIN), (18, 'VBG', PT.PASSIVE), (19, 'AGND', PT.PWRIN), (20, 'TOCAP', PT.PASSIVE),
         (21, 'AVDD', PT.PWRIN), (22, '1V2O', PT.PWROUT), (23, 'RSVD', PT.NOCONNECT), (24, 'SPDLED', PT.OUTPUT),
         (25, 'LINKLED', PT.OUTPUT), (26, 'DUPLED', PT.OUTPUT), (27, 'ACTLED', PT.OUTPUT), (28, 'VDD', PT.PWRIN),
         (29, 'GND', PT.PWRIN), (30, 'XI', PT.INPUT), (31, 'XO', PT.OUTPUT), (32, 'SCSn', PT.INPUT),
         (33, 'SCLK', PT.INPUT), (34, 'MISO', PT.OUTPUT), (35, 'MOSI', PT.INPUT), (36, 'INTn', PT.OUTPUT),
         (37, 'RSTn', PT.INPUT), (38, 'RSVD', PT.NOCONNECT), (39, 'RSVD', PT.NOCONNECT), (40, 'RSVD', PT.NOCONNECT),
         (41, 'RSVD', PT.NOCONNECT), (42, 'RSVD', PT.NOCONNECT), (43, 'PMODE2', PT.INPUT), (44, 'PMODE1', PT.INPUT),
         (45, 'PMODE0', PT.INPUT), (46, 'NC', PT.NOCONNECT), (47, 'NC', PT.NOCONNECT), (48, 'AGND', PT.PWRIN)],
        value='W5500')
U2 = W5(); U2.ref = 'U2'
for n in (3, 9, 14, 16, 19, 48):
    U2[n] += GND          # AGND
U2['GND'] += GND
for n in (4, 8, 11, 15, 17, 21):
    U2[n] += P3V3         # AVDD
U2['VDD'] += P3V3
U2['SCLK'] += SCLK; U2['MOSI'] += MOSI; U2['MISO'] += MISO
U2['SCSn'] += ETH_CS; U2['INTn'] += ETH_INT; U2['RSTn'] += ETH_RST
U2['XI'] += XI; U2['XO'] += XO
U2['1V2O'] += V12; U2['TOCAP'] += TOCAP
U2['TXP'] += ETH_TXP; U2['TXN'] += ETH_TXN; U2['RXP'] += ETH_RXP; U2['RXN'] += ETH_RXN
U2['LINKLED'] += ETH_LL; U2['ACTLED'] += ETH_AL
# PMODE0..2 left to internal pull-ups (all-capable auto-negotiation)
# W5500 support parts
Rset = R('R3', '12k'); Rset[1] += U2['EXRES1']; Rset[2] += GND        # W5500 EXRES1 (12.4k spec; 12k basic, within 100BASE-TX tol)
C12a = C('C4', '1uF'); C12a[1] += V12; C12a[2] += GND
C12b = C('C5', '100nF'); C12b[1] += V12; C12b[2] += GND
Ctoc = C('C6', '4.7uF'); Ctoc[1] += TOCAP; Ctoc[2] += GND             # TOCAP ~4.7uF (datasheet)
# VBG (pin 18): left floating per W5500 datasheet (no cap)
for i, ref in enumerate(['C8', 'C9', 'C10', 'C11']):
    cc = C(ref, '100nF'); cc[1] += P3V3; cc[2] += GND                  # AVDD/VDD decoupling
# 25 MHz crystal
XT = mk('Crystal', 'Y', 'Crystal:Crystal_SMD_2520-4Pin_2.5x2.0mm',
        [(1, '1', PT.PASSIVE), (2, '2', PT.PASSIVE), (3, '3', PT.PASSIVE), (4, '4', PT.PASSIVE)],
        value='25MHz')
Y1 = XT(); Y1.ref = 'Y1'
Y1[1] += XI; Y1[3] += XO; Y1[2] += GND; Y1[4] += GND
Cx1 = C('C12', '22pF'); Cx1[1] += XI; Cx1[2] += GND
Cx2 = C('C13', '22pF'); Cx2[1] += XO; Cx2[2] += GND
# MagJack HR961160C (SMD 10/100, integrated magnetics + link/act LEDs) — in stock, SMD (no THT)
MJ = mk('HR961160C', 'J', 'C55683:RJ45-SMD_HR961160C',
        [(1, 'LED1A', PT.PASSIVE), (2, 'LED1K', PT.PASSIVE), (3, 'TDP', PT.PASSIVE), (4, 'TDN', PT.PASSIVE),
         (5, 'CT', PT.PASSIVE), (6, 'CT', PT.PASSIVE), (7, 'RDP', PT.PASSIVE), (8, 'RDN', PT.PASSIVE),
         (9, 'NC', PT.NOCONNECT), (10, 'CHSGND', PT.PASSIVE), (11, 'LED2K', PT.PASSIVE), (12, 'LED2A', PT.PASSIVE),
         (13, 'SH', PT.PASSIVE), (14, 'SH', PT.PASSIVE)], value='HR961160C')
J3 = MJ(); J3.ref = 'J3'
J3['TDP'] += ETH_TXP; J3['TDN'] += ETH_TXN; J3['RDP'] += ETH_RXP; J3['RDN'] += ETH_RXN
# W5500 TX=current-mode -> TCT(pin5) biased to 3V3 via 49.9R; RX=voltage-mode -> RCT(pin6) to GND via cap (WIZnet ref)
J3[5] += ETH_TCT; J3[6] += ETH_RCT
Rtct = R('R18', '49.9R'); Rtct[1] += ETH_TCT; Rtct[2] += P3V3
Ctct = C('C14', '100nF'); Ctct[1] += ETH_TCT; Ctct[2] += GND
Crct = C('C22', '100nF'); Crct[1] += ETH_RCT; Crct[2] += GND
J3['CHSGND'] += GND; J3[13] += GND; J3[14] += GND
# link/act LEDs in the jack: anode -> 3V3 via R, cathode -> W5500 LED output (active low)
Rll = R('R4', '330R'); Rll[1] += P3V3; Rll[2] += J3['LED1A']; J3['LED1K'] += ETH_LL   # green = link
Ral = R('R5', '330R'); Ral[1] += P3V3; Ral[2] += J3['LED2A']; J3['LED2K'] += ETH_AL   # yellow = act

# ============================================================================
# U3  CH340K USB-UART (auto-reset)  + USB-C data inlet J2
# ============================================================================
CH = mk('CH340C', 'U', 'C84681:SOP-16_L10.0-W3.9-P1.27-LS6.0-BL',
        [(1, 'GND', PT.PWRIN), (2, 'TXD', PT.OUTPUT), (3, 'RXD', PT.INPUT), (4, 'V3', PT.PWROUT),
         (5, 'UDP', PT.PASSIVE), (6, 'UDM', PT.PASSIVE), (7, 'NC', PT.NOCONNECT), (8, 'OUT', PT.OUTPUT),
         (9, 'CTS', PT.INPUT), (10, 'DSR', PT.INPUT), (11, 'RI', PT.INPUT), (12, 'DCD', PT.INPUT),
         (13, 'DTR', PT.OUTPUT), (14, 'RTS', PT.OUTPUT), (15, 'R232', PT.INPUT), (16, 'VCC', PT.PWRIN)],
        value='CH340C')
U3 = CH(); U3.ref = 'U3'
U3['VCC'] += P3V3; U3['V3'] += P3V3; U3['GND'] += GND   # 3.3V operation: V3 tied to VCC (SOP-16, no EP)
U3['UDP'] += USB_DP; U3['UDM'] += USB_DM
U3['TXD'] += S3_RX; U3['RXD'] += S3_TX        # CH340 TXD -> S3 RX, CH340 RXD <- S3 TX
U3['DTR'] += DTR; U3['RTS'] += RTS
Cch = C('C15', '100nF'); Cch[1] += P3V3; Cch[2] += GND
# 2-transistor auto-reset (NodeMCU style): emitters cross to the opposite signal
NPN = mk('MMBT3904', 'Q', 'Package_TO_SOT_SMD:SOT-23',
         [(1, 'B', PT.INPUT), (2, 'E', PT.PASSIVE), (3, 'C', PT.OUTPUT)], value='MMBT3904')
Q1 = NPN(); Q1.ref = 'Q1'; Q2 = NPN(); Q2.ref = 'Q2'
Rq1 = R('R6', '10k'); Rq1[1] += RTS; Rq1[2] += Q1['B']; Q1['E'] += DTR; Q1['C'] += EN
Rq2 = R('R7', '10k'); Rq2[1] += DTR; Rq2[2] += Q2['B']; Q2['E'] += RTS; Q2['C'] += IO0

# USB-C (data) inlet
UC = mk('USB_C', 'J', 'C165948:USB-C_SMD-TYPE-C-31-M-12_1',
        [('A1B12', 'GND', PT.PASSIVE), ('B1A12', 'GND', PT.PASSIVE),
         ('A4B9', 'VBUS', PT.PASSIVE), ('B4A9', 'VBUS', PT.PASSIVE),
         ('A5', 'CC1', PT.PASSIVE), ('B5', 'CC2', PT.PASSIVE),
         ('A6', 'DP1', PT.PASSIVE), ('B6', 'DP2', PT.PASSIVE),
         ('A7', 'DN1', PT.PASSIVE), ('B7', 'DN2', PT.PASSIVE),
         ('A8', 'SBU1', PT.NOCONNECT), ('B8', 'SBU2', PT.NOCONNECT),
         (1, 'SH', PT.PASSIVE), (2, 'SH', PT.PASSIVE), (3, 'SH', PT.PASSIVE), (4, 'SH', PT.PASSIVE)],
        value='USB-C')
J2 = UC(); J2.ref = 'J2'
J2['VBUS'] += P5V; J2['GND'] += GND
J2['DP1'] += USB_DP; J2['DP2'] += USB_DP; J2['DN1'] += USB_DM; J2['DN2'] += USB_DM
J2['CC1'] += CC1; J2['CC2'] += CC2
for p in (1, 2, 3, 4):
    J2[p] += GND          # shield
Rcc1 = R('R8', '5k1'); Rcc1[1] += CC1; Rcc1[2] += GND
Rcc2 = R('R9', '5k1'); Rcc2[1] += CC2; Rcc2[2] += GND

# ============================================================================
# U4  SY8089 5V -> 3.3V buck
# ============================================================================
BK = mk('SY8089', 'U', 'C78988:SOT-23-5_L3.0-W1.7-P0.95-LS2.8-BR',
        [(1, 'EN', PT.INPUT), (2, 'GND', PT.PWRIN), (3, 'LX', PT.OUTPUT), (4, 'IN', PT.PWRIN), (5, 'FB', PT.INPUT)],
        value='SY8089')
U4 = BK(); U4.ref = 'U4'
U4['IN'] += P5V; U4['EN'] += P5V; U4['GND'] += GND; U4['LX'] += LX; U4['FB'] += FB
L1 = mk('L', 'L', 'C354584:IND-SMD_L4.0-W4.0', [(1, '1', PT.PASSIVE), (2, '2', PT.PASSIVE)], value='2.2uH')()
L1.ref = 'L1'; L1[1] += LX; L1[2] += P3V3
Cin = C('C16', '22uF', 'Capacitor_SMD:C_0805_2012Metric'); Cin[1] += P5V; Cin[2] += GND
Cout = C('C17', '22uF', 'Capacitor_SMD:C_0805_2012Metric'); Cout[1] += P3V3; Cout[2] += GND
Rfb1 = R('R10', '45.3k'); Rfb1[1] += P3V3; Rfb1[2] += FB     # FB divider: 0.6*(1+45.3/10)=3.32V (YAGEO RC0402FR-0745K3L / C137977)
Rfb2 = R('R11', '10k'); Rfb2[1] += FB; Rfb2[2] += GND

# ============================================================================
# Isolated DMX:  U5 ISO3086DWR (iso RS-485) + PS1 B0505S iso-DCDC + J1 XLR-3 + protection
# ============================================================================
ISO = mk('ISO3086DWR', 'U', 'C183095:SOIC-16_L10.3-W7.5-P1.27-LS10.3-BL',
         [(1, 'VCC1', PT.PWRIN), (2, 'GND1', PT.PWRIN), (3, 'R', PT.OUTPUT), (4, 'nRE', PT.INPUT),
          (5, 'DE', PT.INPUT), (6, 'D', PT.INPUT), (7, 'GND1', PT.PWRIN), (8, 'GND1', PT.PWRIN),
          (9, 'GND2', PT.PWRIN), (10, 'GND2', PT.PWRIN), (11, 'Y', PT.OUTPUT), (12, 'Z', PT.OUTPUT),
          (13, 'B', PT.BIDIR), (14, 'A', PT.BIDIR), (15, 'GND2', PT.PWRIN), (16, 'VCC2', PT.PWRIN)],
         value='ISO3086DWR')
U5 = ISO(); U5.ref = 'U5'
U5['VCC1'] += P3V3; U5[2] += GND; U5[7] += GND; U5[8] += GND
U5['R'] += DMX_RX; U5['D'] += DMX_TX; U5['nRE'] += DMX_EN; U5['DE'] += DMX_EN
U5['VCC2'] += VISO; U5[9] += GND2; U5[10] += GND2; U5[15] += GND2
U5['Y'] += DMX_A; U5['A'] += DMX_A; U5['Z'] += DMX_B; U5['B'] += DMX_B   # 2-wire: Y-A=Data+, Z-B=Data-
Ciso1 = C('C18', '100nF'); Ciso1[1] += P3V3; Ciso1[2] += GND
Ciso2 = C('C19', '100nF'); Ciso2[1] += VISO; Ciso2[2] += GND2
Ciso3 = C('C20', '10uF', 'Capacitor_SMD:C_1206_3216Metric'); Ciso3[1] += VISO; Ciso3[2] += GND2

PSU = mk('B0505S-1W', 'PS', 'C2912568:PWRM-TH_B0505S-1W',
         [(1, 'GNDin', PT.PWRIN), (2, 'VIN', PT.PWRIN), (3, 'GNDout', PT.PWROUT), (4, 'VOUT', PT.PWROUT)],
         value='B0505S-1W')
PS1 = PSU(); PS1.ref = 'PS1'
PS1['VIN'] += P5V; PS1['GNDin'] += GND; PS1['VOUT'] += VISO; PS1['GNDout'] += GND2
Cb = C('C21', '10uF', 'Capacitor_SMD:C_1206_3216Metric'); Cb[1] += P5V; Cb[2] += GND

# DMX termination + TVS protection + XLR-3
Rterm = R('R12', '120R', 'Resistor_SMD:R_0805_2012Metric'); Rterm[1] += DMX_A; Rterm[2] += DMX_B
TVS = mk('SM712', 'D', 'Package_TO_SOT_SMD:SOT-23',
         [(1, 'IO1', PT.PASSIVE), (2, 'GND', PT.PASSIVE), (3, 'IO2', PT.PASSIVE)], value='SM712')
D1 = TVS(); D1.ref = 'D1'; D1['IO1'] += DMX_A; D1['GND'] += GND2; D1['IO2'] += DMX_B
XLR = mk('XLR3', 'J', 'XLR328P:CONN-TH_XLR-328P',
         [(1, 'SHIELD', PT.PASSIVE), (2, 'DATA-', PT.PASSIVE), (3, 'DATA+', PT.PASSIVE)],
         value='XLR-3 out')
J1 = XLR(); J1.ref = 'J1'
J1['SHIELD'] += GND2; J1['DATA-'] += DMX_B; J1['DATA+'] += DMX_A

# ============================================================================
# Status LEDs (direct on S3 GPIOs): GPIO -> R -> LED anode, cathode -> GND
# ============================================================================
LEDP = mk('LED', 'D', 'LED_SMD:LED_0603_1608Metric',
          [(1, 'K', PT.PASSIVE), (2, 'A', PT.PASSIVE)], value='LED')
for ref, net, rref, rval in [('D2', LED_R, 'R13', '1k'), ('D3', LED_G, 'R14', '150R'),
                             ('D4', LED_Y, 'R15', '1k'), ('D5', LED_B, 'R16', '150R'),
                             ('D6', LED_W, 'R17', '150R')]:
    d = LEDP(); d.ref = ref
    r = R(rref, rval)
    r[1] += net; r[2] += d['A']; d['K'] += GND

# ============================================================================
# J4  Display breakout header (optional OLED/TFT status panel — issue #5, docs/display.md)
#   I2C mono OLED (SSD1306/SH1106): SDA=IO4  SCL=IO5
#   color SPI OLED/TFT (SSD1351/ST7789): SCK=IO39 MOSI=IO40 CS=IO41 DC=IO42 RST=IO38
#   NB: v3 uses these FREE GPIOs, NOT the doc's S3 default 8/9 (here 8=DMX_DE, 9=ETH_RST).
#   Firmware must set dispSda/dispScl (+ SPI pins) in /config to match this header.
# ============================================================================
DISP_SDA = Net('DISP_SDA'); DISP_SCL = Net('DISP_SCL')
DISP_SCK = Net('DISP_SCK'); DISP_MOSI = Net('DISP_MOSI'); DISP_CS = Net('DISP_CS')
DISP_DC  = Net('DISP_DC');  DISP_RST  = Net('DISP_RST')
U1['IO4']  += DISP_SDA;  U1['IO5']  += DISP_SCL
U1['IO39'] += DISP_SCK;  U1['IO40'] += DISP_MOSI; U1['IO41'] += DISP_CS
U1['IO42'] += DISP_DC;   U1['IO38'] += DISP_RST
HDR = mk('Conn_JSTSH9', 'J', 'Connector_JST:JST_SH_SM09B-SRSS-TB_1x09-1MP_P1.00mm_Horizontal',
         [(i, str(i), PT.PASSIVE) for i in range(1, 10)] + [('MP', 'MP', PT.PASSIVE)], value='DISP SH9')
J4 = HDR(); J4.ref = 'J4'
# JST SH 1.0mm 9-pin SMD side-entry (~10mm wide; pre-crimped cables available):
#   1 +3V3  2 GND  3 SDA  4 SCL  5 SCK  6 MOSI  7 CS  8 DC  9 RST
J4[1] += P3V3;      J4[2] += GND;       J4[3] += DISP_SDA; J4[4] += DISP_SCL; J4[5] += DISP_SCK
J4[6] += DISP_MOSI; J4[7] += DISP_CS;   J4[8] += DISP_DC;  J4[9] += DISP_RST
J4['MP'] += GND     # mounting tabs -> GND (mechanical anchor + shield)

ERC()
generate_netlist(file_=os.path.join(HERE, 'lumigate.net'))
print('NETLIST GENERATED OK ->', os.path.join(HERE, 'lumigate.net'))
