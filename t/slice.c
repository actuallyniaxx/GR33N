#include <stdio.h>
#include <string.h>
typedef unsigned char u8; typedef unsigned int u32;
static int fails=0;
#define CHECK(c,...) do{ if(!(c)){printf("FALLO: ");printf(__VA_ARGS__);printf("\n");fails++;} }while(0)
static u32 utf8_next(const char **pp){
	const u8 *p=(const u8*)*pp; u32 cp;
	if(p[0]<0x80){*pp=(const char*)(p+1);return p[0];}
	if((p[0]&0xE0)==0xC0&&(p[1]&0xC0)==0x80){cp=((u32)(p[0]&0x1F)<<6)|(u32)(p[1]&0x3F);*pp=(const char*)(p+2);return cp;}
	if((p[0]&0xF0)==0xE0){*pp=(const char*)(p+3);return 0;}
	if((p[0]&0xF8)==0xF0){*pp=(const char*)(p+4);return 0;}
	*pp=(const char*)(p+1);return 0;
}
static int textSlice(char *out,u32 outsize,const char *src,int from_cp,int n_cp){
	const char *p=src; const char *start; u32 used=0; int i,got=0;
	if(!out||outsize==0)return 0; out[0]='\0'; if(!src||n_cp<=0)return 0;
	for(i=0;i<from_cp&&*p;i++)utf8_next(&p);
	start=p;
	while(*p&&got<n_cp){const char *before=p;u32 len;utf8_next(&p);len=(u32)(p-before);
		if(used+len+1>outsize)break; used+=len; got++;}
	memcpy(out,start,used); out[used]='\0'; return got;
}
static int textLen(const char *s){int n=0;if(!s)return 0;while(*s){utf8_next(&s);n++;}return n;}
int main(void)
{
	char o[64];
	/* nombre con acentos: cada caracter acentuado son DOS bytes */
	const char *acc = "Ori y la Voluntad de las Lumbrías";   /* í = 2 bytes */
	int n = textLen(acc);
	CHECK(n == 33, "codepoints de la cadena con acento: %d", n);
	CHECK((int)strlen(acc) == 34, "bytes: %d (uno mas que codepoints)", (int)strlen(acc));

	/* una ventana que cae JUSTO encima del acento no debe partirlo */
	{
		int i;
		for (i = 0; i <= n; i++) {
			int got = textSlice(o, sizeof o, acc, i, 13);
			/* toda salida tiene que ser UTF-8 valido: recorrerla no
			 * puede pasarse del final */
			const char *p = o; int cps = 0;
			while (*p) { utf8_next(&p); cps++; }
			CHECK(p == o + strlen(o), "desde %d: la ventana parte una secuencia", i);
			CHECK(cps == got, "desde %d: dice %d y tiene %d", i, got, cps);
		}
	}

	CHECK(textSlice(o, sizeof o, "007 First Light", 0, 13) == 13, "ascii 13");
	CHECK(strcmp(o, "007 First Lig") == 0, "ventana ascii = '%s'", o);
	CHECK(textSlice(o, sizeof o, "007 First Light", 4, 13) == 11, "cola mas corta");
	CHECK(strcmp(o, "First Light") == 0, "cola = '%s'", o);
	CHECK(textSlice(o, sizeof o, "abc", 99, 5) == 0, "mas alla del final");
	CHECK(o[0] == '\0', "y deja cadena vacia");
	CHECK(textSlice(o, 4, "abcdefgh", 0, 8) == 3, "buffer pequeno corta a 3");
	/* buffer que no da ni para un caracter de dos bytes */
	CHECK(textSlice(o, 2, "ñx", 0, 2) == 0, "no cabe medio caracter");
	CHECK(o[0] == '\0', "y no escribe medio");

	printf(fails?"%d FALLOS\n":"textSlice: todo correcto (%d fallos)\n",fails);
	return fails?1:0;
}
