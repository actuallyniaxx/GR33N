#---------------------------------------------------------------------------------
# GR33N - cliente de streaming para PS3 (PSL1GHT)
#
#   make          -> gr33n.self  (firmado CEX) + gr33n.fake.self
#   make pkg      -> gr33n.pkg   (instalable) + gr33n.gnpdrm.pkg
#   make run      -> ps3load gr33n.self  (a la consola por red)
#   make clean
#
# Necesita PS3DEV y PSL1GHT en el entorno:
#   export PS3DEV=/usr/local/ps3dev
#   export PSL1GHT=$PS3DEV
#   export PATH=$PATH:$PS3DEV/bin:$PS3DEV/ppu/bin:$PS3DEV/spu/bin
#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------
ifeq ($(strip $(PSL1GHT)),)
$(error "PSL1GHT no esta definido. export PSL1GHT=/usr/local/ps3dev")
endif

include $(PSL1GHT)/ppu_rules

#---------------------------------------------------------------------------------
TARGET		:=	gr33n
BUILD		:=	build
SOURCES		:=	source deps/cjson
DATA		:=	data
SHADERS		:=	shaders
INCLUDES	:=	include deps/cjson

TITLE		:=	GR33N
# Nueve caracteres alfanumericos, que es lo que usa el homebrew de PS3
# (NP00PKGI3, PKGLAUNCH, RELOADXMB...). El formato de cuatro letras y
# cinco digitos es de titulos retail, no se aplica aqui. Tiene que
# coincidir con la carpeta de /dev_hdd0/game/ y con el TITLE_ID del
# PARAM.SFO.
APPID		:=	GR33N0PS3
CONTENTID	:=	UP0001-$(APPID)_00-0000000000000000

# Recursos de la entrada del XMB. Ver RECURSOS-XMB.txt (en la raiz) para
# los nombres y los tamanos exactos.
PKGFILES	:=	pkgfiles

ifneq ($(wildcard $(CURDIR)/pkgfiles/ICON0.PNG),)
ICON0		:=	$(CURDIR)/pkgfiles/ICON0.PNG
endif

# NUESTRA plantilla de PARAM.SFO, no la generica de PSL1GHT. Ahi vive
# CATEGORY = CB, que es lo que hace que GR33N salga en la columna de RED
# del XMB y no entre los juegos. Es un cliente de streaming: no es un
# juego, es una aplicacion de red.
# OJO: sfo.xml va en la RAIZ, no en pkgfiles/. La regla de PSL1GHT copia
# pkgfiles/* ENTERO dentro del paquete, asi que ahi solo pueden vivir los
# recursos que el XMB va a leer. Un sfo.xml o un README colados ahi acaban
# instalados en la consola.
ifneq ($(wildcard $(CURDIR)/sfo.xml),)
SFOXML		:=	$(CURDIR)/sfo.xml
endif

#---------------------------------------------------------------------------------
# Flags. -O2 no esta en los samples de PSL1GHT y se nota: el patron de
# prueba escribe 3,5 MB por frame, sin optimizar no llega a 60.
#---------------------------------------------------------------------------------
# mbedTLS. La ruta la fija deps/build-mbedtls.sh; se puede pisar con
# make MBEDTLS=/otra/ruta
MBEDTLS		?=	$(HOME)/.gr33n-deps/mbedtls-ps3

# Y las otras tres de WebRTC, con la misma forma y las mismas rutas que
# dejan sus scripts. Se pueden pisar igual:  make LIBPEER=/otra/ruta
#
# EL ORDEN EN QUE SE CONSTRUYEN no es el mismo que el de enlazado:
# mbedtls -> libsrtp -> usrsctp -> libpeer, porque cada una necesita las
# cabeceras de las anteriores.
LIBSRTP		?=	$(HOME)/.gr33n-deps/libsrtp-ps3
USRSCTP		?=	$(HOME)/.gr33n-deps/usrsctp-ps3
LIBPEER		?=	$(HOME)/.gr33n-deps/libpeer-ps3

# Y opus, que la deja deps/build-opus.sh con la misma forma.
# No depende de ninguna de las otras: solo la llama aud.c.
OPUS		?=	$(HOME)/.gr33n-deps/opus-ps3

# CRITICO: la MISMA definicion con la que se compilo la biblioteca.
# mbedTLS decide el tamano de sus estructuras segun la configuracion, asi
# que si la app ve una configuracion y la biblioteca otra, los structs no
# coinciden y se corrompe memoria en silencio. No es un aviso teorico: es
# el fallo clasico de integrar mbedTLS a mano.
MBEDTLS_DEF	=	-DMBEDTLS_USER_CONFIG_FILE='"ps3_mbedtls_config.h"'

CFLAGS		=	-O2 -Wall -Wextra -std=gnu99 -mcpu=cell $(MACHDEP) $(INCLUDE) $(MBEDTLS_DEF)
CXXFLAGS	=	$(CFLAGS)
LDFLAGS		=	$(MACHDEP) -Wl,-Map,$(notdir $@).map

#---------------------------------------------------------------------------------
# EL ORDEN DE ESTA LINEA ES SIGNIFICATIVO.
#
# El enlazador de GNU recorre las bibliotecas UNA VEZ y de izquierda a
# derecha: de cada una solo se lleva los objetos que resuelven simbolos que
# ya ha visto pendientes. Asi que cada biblioteca va DELANTE de aquellas de
# las que depende.
#
#   libpeer    necesita  srtp2, usrsctp y mbedtls
#   libsrtp2   necesita  mbedtls
#   libusrsctp necesita  pthreads y libc de la consola
#
# Si -lmbedtls fuera antes que -lpeer, el enlazador pasaria por mbedTLS sin
# saber todavia que libpeer va a pedirle mbedtls_ssl_setup, y luego se
# quejaria de un simbolo que esta ahi mismo. Es el fallo de enlace mas
# desconcertante que hay: el error dice "undefined reference" de algo que se
# ve en la biblioteca con nm.
#
# -laudio es de PSL1GHT (el puerto de sonido del sistema) y -lopus la
# nuestra. Van SUELTAS y no encajan en la cadena de arriba porque no
# dependen de nadie ni nadie depende de ellas: aud.c llama a las dos y ahi
# se acaba. -lopus antes de -lm porque usa sqrt en un par de sitios.
LIBS	:=	-lrsx -lgcm_sys -lio -lsysutil -lvdec -lpngdec -ljpgdec \
			-lpeer -lsrtp2 -lusrsctp -lmbedtls -lpthread \
			-laudio -lopus \
			-lnet -lnetctl -lsysmodule -lrt -llv2 -lm
LIBDIRS	:=	$(MBEDTLS) $(LIBSRTP) $(USRSCTP) $(LIBPEER) $(OPUS)

#---------------------------------------------------------------------------------
# A partir de aqui es el boilerplate estandar de PSL1GHT.
#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT	:=	$(CURDIR)/$(TARGET)

export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
					$(foreach dir,$(DATA),$(CURDIR)/$(dir)) \
					$(foreach dir,$(SHADERS),$(CURDIR)/$(dir))

export DEPSDIR	:=	$(CURDIR)/$(BUILD)
export BUILDDIR	:=	$(CURDIR)/$(BUILD)

CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
sFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
SFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.S)))
BINFILES	:=	$(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))
VCGFILES	:=	$(foreach dir,$(SHADERS),$(notdir $(wildcard $(dir)/*.vcg)))
FCGFILES	:=	$(foreach dir,$(SHADERS),$(notdir $(wildcard $(dir)/*.fcg)))

VPOFILES	:=	$(VCGFILES:.vcg=.vpo)
FPOFILES	:=	$(FCGFILES:.fcg=.fpo)

ifeq ($(strip $(CPPFILES)),)
	export LD	:=	$(CC)
else
	export LD	:=	$(CXX)
endif

export OFILES	:=	$(addsuffix .o,$(BINFILES)) \
					$(addsuffix .o,$(VPOFILES)) \
					$(addsuffix .o,$(FPOFILES)) \
					$(CPPFILES:.cpp=.o) $(CFILES:.c=.o) \
					$(sFILES:.s=.o) $(SFILES:.S=.o)

export INCLUDE	:=	$(foreach dir,$(INCLUDES), -I$(CURDIR)/$(dir)) \
					$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
					$(LIBPSL1GHT_INC) \
					-I$(CURDIR)/$(BUILD)

export LIBPATHS	:=	$(foreach dir,$(LIBDIRS),-L$(dir)/lib) \
					$(LIBPSL1GHT_LIB)

.PHONY: $(BUILD) clean run pkg

#---------------------------------------------------------------------------------
$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

#---------------------------------------------------------------------------------
clean:
	@echo limpiando ...
	@rm -fr $(BUILD) $(OUTPUT).elf $(OUTPUT).self $(OUTPUT).fake.self \
		$(OUTPUT).pkg $(OUTPUT).gnpdrm.pkg $(OUTPUT).elf.map

#---------------------------------------------------------------------------------
run: $(BUILD)
	ps3load $(OUTPUT).self

#---------------------------------------------------------------------------------
# El paso pkg-assets va ANTES a proposito y en un make aparte, para que
# el orden este garantizado.
pkg: $(BUILD)
	@$(MAKE) --no-print-directory pkg-assets
	@$(MAKE) --no-print-directory $(OUTPUT).pkg

#---------------------------------------------------------------------------------
# Revisa los recursos del XMB antes de empaquetar.
#
# EXISTE POR UN FALLO REAL: ICON0 aparecia en el XMB y PIC0/PIC1 no. El
# motivo era el nombre. La regla de PSL1GHT hace
#
#     cp $(ICON0) build/pkg/ICON0.PNG        <- RENOMBRA a mayusculas
#     cp -rf pkgfiles/* build/pkg/           <- copia tal cual
#
# asi que un ICON0.png en minusculas llegaba bien y un PIC1.png llegaba
# como PIC1.png. El XMB busca PIC1.PNG y no mira otra cosa.
#
# Y no se ve venir: Windows guarda el nombre como lo escribiste pero no
# distingue mayusculas al buscar, asi que en el PC todo parece correcto.
# El renombrado va en dos pasos por eso mismo: un mv directo cree que
# origen y destino son el mismo fichero.
.PHONY: pkg-assets
pkg-assets:
	@for f in ICON0 PIC0 PIC1; do \
		if [ -f $(PKGFILES)/$$f.png ]; then \
			echo "  $$f.png -> $$f.PNG (el XMB solo lee la extension en mayusculas)"; \
			mv $(PKGFILES)/$$f.png $(PKGFILES)/$$f.rename.tmp; \
			mv $(PKGFILES)/$$f.rename.tmp $(PKGFILES)/$$f.PNG; \
		fi; \
	done
	@if [ -f $(PKGFILES)/SND0.at3 ]; then \
		mv $(PKGFILES)/SND0.at3 $(PKGFILES)/SND0.rename.tmp; \
		mv $(PKGFILES)/SND0.rename.tmp $(PKGFILES)/SND0.AT3; \
	fi
	@for f in $(PKGFILES)/*.PNG; do \
		[ -f "$$f" ] || continue; \
		il=`od -An -tu1 -j28 -N1 "$$f" | tr -d ' '`; \
		if [ "$$il" != "0" ]; then \
			echo "  AVISO: $$f esta ENTRELAZADO. El XMB no lo va a dibujar."; \
			echo "         magick $$f -interlace none $$f"; \
		fi; \
	done
	@for f in $(PKGFILES)/*.txt $(PKGFILES)/*.xml; do \
		[ -f "$$f" ] || continue; \
		echo "  AVISO: $$f acabara DENTRO del paquete. pkgfiles/ es solo"; \
		echo "         para recursos que lea el XMB."; \
	done
# La version del PARAM.SFO se escribe a mano y la del codigo no, asi que
# se separan solas y nadie se entera hasta que alguien mira la ficha en el
# XMB y ve 01.00 en una 0.14. Comparar mayor.menor es todo lo que se puede
# hacer: el formato NN.NN del SFO no da para el numero de parche.
	@if [ -f $(CURDIR)/sfo.xml ] && [ -f $(CURDIR)/include/gr33n.h ]; then \
		sv=`sed -n 's/.*APP_VER[^>]*>\([0-9.]*\)<.*/\1/p' $(CURDIR)/sfo.xml | head -1`; \
		cv=`sed -n 's/.*GR33N_VERSION[^"]*"\([0-9]*\)\.\([0-9]*\).*/\1 \2/p' $(CURDIR)/include/gr33n.h | head -1`; \
		want=`echo $$cv | awk '{ printf "%02d.%02d", $$1, $$2 }'`; \
		if [ -n "$$want" ] && [ "$$sv" != "$$want" ]; then \
			echo "  AVISO: sfo.xml dice APP_VER $$sv y el codigo va por $$want."; \
			echo "         Actualiza APP_VER y VERSION en sfo.xml."; \
		fi; \
	fi

#---------------------------------------------------------------------------------
else

DEPENDS	:=	$(OFILES:.o=.d)

$(OUTPUT).self: $(OUTPUT).elf
$(OUTPUT).elf:	$(OFILES)

#---------------------------------------------------------------------------------
%.bin.o	:	%.bin
	@echo $(notdir $<)
	@$(bin2o)

# Los clips H.264 de data/ se empotran en el binario igual que
# cualquier otro dato. bin2s genera clip_h264[] / clip_h264_end[].
%.h264.o	:	%.h264
	@echo $(notdir $<)
	@$(bin2o)

%.vpo.o	:	%.vpo
	@echo $(notdir $<)
	@$(bin2o)

%.fpo.o	:	%.fpo
	@echo $(notdir $<)
	@$(bin2o)

-include $(DEPENDS)

#---------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------
