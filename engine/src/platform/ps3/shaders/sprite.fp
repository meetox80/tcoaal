!!ARBfp1.0
OPTION NV_fragment_program2;
OPTION ARB_precision_hint_fastest;
#var float4 color : $vin.COLOR0 : COL0 : 0 : 1
#var float2 texcoord : $vin.TEXCOORD0 : TEX0 : 1 : 1
#var sampler2D texture :  : texunit 0 : 2 : 1
#var float4 oColor : $vout.COLOR0 : COL : 3 : 1
SHORT TEMP H0;
TEMP RC;
TEMP HC;
OUTPUT oCol = result.color;
TEX   H0, fragment.texcoord[0], texture[0], 2D;
MULH  oCol, H0, fragment.color.primary;
END
