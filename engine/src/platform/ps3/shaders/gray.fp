!!ARBfp1.0
OPTION NV_fragment_program2;
OPTION ARB_precision_hint_fastest;
#var float4 color : $vin.COLOR0 : COL0 : 0 : 1
#var float2 texcoord : $vin.TEXCOORD0 : TEX0 : 1 : 1
#var sampler2D texture :  : texunit 0 : 2 : 1
#var float4 oColor : $vout.COLOR0 : COL : 3 : 1
#const c[0] = 0.33333334 1
PARAM c[1] = { { 0.33333334, 1 } };
SHORT TEMP H0;
SHORT TEMP H1;
TEMP RC;
TEMP HC;
OUTPUT oCol = result.color;
TEX   H0.xyz, fragment.texcoord[0], texture[0], 2D;
DP3H  H0.w, H0, c[0].x;
ADDH  H1.xyz, H0.w, -H0;
MADH  oCol.xyz, fragment.color.primary.w, H1, H0;
MOVH  oCol.w, c[0].y;
END
