/* GR33N - soporte de tres idiomas (Español, English, Português) */

#include <string.h>

#include "i18n.h"

static gr33nIdioma actual = IDIOMA_PT;

/* Una sola tabla, con el codigo y el nombre juntos. */
static const struct {
	const char *codigo;
	const char *nombre;
} idiomas[IDIOMA_N] = {
	{ "es", "Español" },
	{ "en", "English" },
	{ "pt", "Português" }
};

static const struct {
	const char *es;
	const char *pt;
} pt_dict[] = {
	/* Pantalla de login / auth (main.c) */
	{ "Iniciar sesion", "Entrar" },
	{ "Ya hay una sesion guardada en esta consola. Si entras con otra cuenta, el token anterior se sobreescribira.",
	  "Já existe uma sessão salva neste console. Se entrar com outra conta, o token anterior será substituído." },
	{ "Pulsa X y la consola te dara un codigo corto para escribir en microsoft.com/link desde el movil o el PC.",
	  "Pressione X e o console fornecerá um código curto para digitar em microsoft.com/link pelo celular ou PC." },
	{ "X  Empezar", "X  Iniciar" },
	{ "Pidiendo codigo...", "Solicitando código..." },
	{ "1.  Entra en esta direccion:", "1.  Acesse este endereço:" },
	{ "2.  Escribe este codigo:", "2.  Digite este código:" },
	{ "Esperando...   %u s de %u   %u consultas", "Aguardando...   %u s de %u   %u consultas" },
	{ "Sesion iniciada", "Sessão iniciada" },
	{ "El token queda guardado en la consola. En el proximo arranque no hara falta volver a iniciar sesion.",
	  "Login concluído! O token foi salvo no console. Nas próximas vezes o login será automático." },
	{ "No se pudo entrar", "Falha ao entrar" },
	{ "X  Reintentar", "X  Tentar novamente" },

	/* Pestañas principales (ui.c) */
	{ "Juegos", "Jogos" },
	{ "Favoritos", "Favoritos" },
	{ "Ajustes", "Configurações" },
	{ "Pruebas", "Testes" },

	/* Estados de cuenta y red */
	{ "Automatico", "Automático" },
	{ "entrando...", "conectando..." },
	{ "sesion iniciada", "conectado" },
	{ "sin iniciar", "não conectado" },
	{ "midiendo", "medindo" },
	{ "mejor:", "melhor:" },
	{ "ninguna contesto", "nenhum respondeu" },
	{ "medir", "testar" },
	{ "hace falta cuenta", "necessário login" },

	/* Catalogo y biblioteca */
	{ "Todos", "Todos" },
	{ "En tu suscripcion", "Na sua assinatura" },
	{ "Ningun juego disponible con tu suscripcion.", "Nenhum jogo disponível na sua assinatura." },
	{ "Jugar", "Jogar" },
	{ "No incluido en tu suscripcion", "Não incluso na sua assinatura" },
	{ "Este juego no trae descripcion.", "Este jogo não possui descrição." },
	{ "Cargando descripcion...", "Carregando descrição..." },
	{ "Sin derecho a este juego: no se pide maquina.", "Jogo fora da sua assinatura: máquina não solicitada." },
	{ "Pidiendo una maquina a xCloud...", "Solicitando máquina ao xCloud..." },
	{ "En cola: %s (unos %u s)", "Na fila: %s (~%u s)" },
	{ "En cola: %s", "Na fila: %s" },
	{ "Diciendole a Microsoft quien eres...", "Autenticando sessão com a Microsoft..." },
	{ "Preparando la maquina: %s (%u)", "Preparando a máquina: %s (%u)" },
	{ "Pidiendo la configuracion...", "Obtendo configurações..." },
	{ "Maquina lista en %u ms (%s:%u es relleno). Falta el video.",
	  "Máquina pronta em %u ms (%s:%u de espera). Aguardando vídeo..." },
	{ "Maquina lista en %u ms. Falta WebRTC: mira el log.",
	  "Máquina pronta em %u ms. Conectando WebRTC: veja o log." },
	{ "Sin juegos", "Sem jogos" },
	{ "Todavia no has marcado ningun favorito. Triangulo sobre un juego.",
	  "Você ainda não marcou nenhum favorito. Pressione Triângulo sobre um jogo para favoritar." },
	{ "Ningun juego con ese filtro. Cuadrado para cambiarlo.",
	  "Nenhum jogo com este filtro. Pressione Quadrado para alterá-lo." },
	{ "Cargando biblioteca...", "Carregando biblioteca..." },
	{ "Inicia sesion en Ajustes para ver tus juegos.", "Entre na sua conta em Configurações para ver seus jogos." },
	{ "No hay ningun juego disponible para lanzar.", "Nenhum jogo disponível para iniciar." },
	{ "Nada que mostrar", "Nada a exibir" },

	/* Barra inferior / Ayuda de controles */
	{ "%s Lanzar    Arriba/Abajo Mover    L1/R1 Pestana%s",
	  "%s Iniciar    Cima/Baixo Mover    L1/R1 Abas%s" },
	{ "    START+SELECT Salir", "    START+SELECT Sair" },
	{ "Izq/Der Cambiar    Arriba/Abajo Mover    L1/R1 Pestana%s",
	  "Esq/Dir Alterar    Cima/Baixo Mover    L1/R1 Abas%s" },
	{ "Stick der. Descripcion    /\\ Favorito    %s Volver",
	  "Analógico Dir. Descrição    /\\ Favorito    %s Voltar" },
	{ "%s Ficha    [] %s    /\\ Favorito    %u juegos    L1/R1 Pestana",
	  "%s Detalhes    [] %s    /\\ Favorito    %u jogos    L1/R1 Abas" },

	/* Atajos de salida */
	{ "SELECT + Atras", "SELECT + Voltar" },
	{ "SELECT + START", "SELECT + START" },
	{ "L1 + R1 + START", "L1 + R1 + START" },
	{ "L3 + R3", "L3 + R3" },
	{ "SELECT + %s / SELECT + START para volver", "SELECT + %s / SELECT + START para voltar" },
	{ "%s / SELECT + START para volver", "%s / SELECT + START para voltar" },
	{ "SELECT + %s para volver", "SELECT + %s para voltar" },
	{ "%s para volver", "%s para voltar" },

	/* Resoluciones y FPS */
	{ "480p (Fluido / Wi-Fi)", "480p (Fluido / Wi-Fi)" },
	{ "720p (Recomendado)", "720p (Recomendado)" },
	{ "1080p (Alta definicion)", "1080p (Alta Definição)" },
	{ "30 FPS (Estable Wi-Fi)", "30 FPS (Estável Wi-Fi)" },
	{ "60 FPS (Original)", "60 FPS (Original)" },

	/* Posiciones de HUD */
	{ "Arriba a la izquierda", "Superior Esquerdo" },
	{ "Arriba a la derecha", "Superior Direito" },
	{ "Abajo a la izquierda", "Inferior Esquerdo" },
	{ "Abajo a la derecha", "Inferior Direito" },

	/* Nombres de los Ajustes (settings) */
	{ "Cuenta", "Conta Microsoft" },
	{ "Entra con tu cuenta de Microsoft. La consola te da un codigo corto y tu lo escribes en el movil: la contrasena no pasa por aqui en ningun momento, que es justo para lo que sirve este metodo. Lo que se guarda en la PS3 es un permiso que caduca y que puedes retirar cuando quieras desde tu cuenta.",
	  "Entre com sua conta Microsoft. O console exibe um código curto para você digitar no celular ou PC: sua senha nunca passa pelo PS3. Fica salvo apenas um token de autorização que você pode revogar quando quiser." },

	{ "Idioma", "Idioma dos Menus" },
	{ "El idioma de GR33N. El de los juegos es el ajuste de abajo. Solo hay dos porque la fuente esta dibujada a mano, pixel a pixel: llega para el castellano y el ingles, y anadir ruso o japones seria dibujar un alfabeto entero.",
	  "O idioma da interface do GR33N. O idioma dos jogos é configurado na opção logo abaixo." },

	{ "Idioma de los juegos", "Idioma dos Jogos" },
	{ "En que idioma arrancan los juegos: voces, textos y teclado. Tambien decide el idioma de los nombres y descripciones de la biblioteca. Ojo, que pedirlo no lo garantiza: un juego que no este doblado saldra en el idioma que tenga, normalmente ingles. Se aplica a la proxima partida, no a la que ya este en marcha.",
	  "Idioma em que os jogos iniciam: dublagem, textos e teclado. Também define o idioma dos nomes e descrições da biblioteca. Aplica-se à próxima partida." },

	{ "Zona muerta", "Zona Morta dos Analógicos" },
	{ "Cuanto hay que mover el stick antes de que cuente. Los mandos con anos encima no vuelven del todo al centro: si el cursor se va solo, sube esto; si los movimientos suaves no responden, bajalo. Abajo tienes los cuatro ejes en vivo -- suelta el mando y sube el valor hasta que se queden todos en verde.",
	  "Sensibilidade inicial dos analógicos para evitar drift em controles desgastados. Solte o controle e ajuste o valor até que todos os eixos fiquem verdes." },

	{ "Intercambiar X y O", "Inverter X e O" },
	{ "En PS3 se acepta con X y se cancela con O. Si vienes de una consola donde es al reves, o de un mando de Xbox, esto lo cambia en todo el menu. A los juegos les sigue llegando lo que pulses de verdad.",
	  "No PS3 confirma-se com X e volta com O. Se você prefere o padrão japonês ou de outros controles, esta opção inverte as ações nos menus." },

	{ "Servidor", "Servidor / Região" },
	{ "A que centro de datos de xCloud te conectas. Elegir el que te pilla lejos puede costarte 80 ms antes de empezar a jugar. La lista te la da Microsoft al iniciar sesion, asi que sin cuenta aqui solo veras Automatico. Los milisegundos son lo que tarda en contestarte la region: no son la latencia jugando, pero son buena parte de ella.",
	  "Datacenter do xCloud ao qual você se conecta. O Brasil possui servidores locais (South Brazil) com menor ping. Sem login, exibirá apenas Automático." },

	{ "Medir latencia", "Testar Latência" },
	{ "Vuelve a medir todas las regiones. Se hace solo al iniciar sesion; esto es para repetirlo cuando cambie la red, cuando alguien se ponga a descargar algo, o simplemente por curiosidad. Tres medidas por region y se queda la mejor, porque el ruido de red solo suma.",
	  "Mede novamente a latência (ping) para todas as regiões do xCloud para encontrar a melhor rota de conexão." },

	{ "Resolucion de streaming", "Resolução de Streaming" },
	{ "Resolucion del video que manda xCloud. 480p consume mucho menos ancho de banda y es ideal para conexiones Wi-Fi con interferencias; 720p es el estandar optimo en PS3; 1080p ofrece la maxima nitidez. Se aplica a la proxima partida.",
	  "Resolução do vídeo transmitido pelo xCloud. 480p consome muito menos banda e é ideal para Wi-Fi com interferências; 720p é o padrão ideal no PS3; 1080p oferece a máxima nitidez. Aplica-se à próxima partida." },

	{ "Fotogramas por segundo (FPS)", "Taxa de Quadros (FPS)" },
	{ "Tasa de cuadros por segundo del stream. 30 FPS reduce el trafico UDP a la mitad y alivia el decodificador, garantizando fluidez total sin tirones en Wi-Fi. 60 FPS ofrece maxima suavidad. Se aplica a la proxima partida.",
	  "Taxa de quadros do stream. 30 FPS reduz o tráfego UDP pela metade e alivia o decodificador Cell VDEC, garantindo estabilidade no Wi-Fi. 60 FPS oferece máxima suavidade. Aplica-se à próxima partida." },

	{ "Mostrar juegos no disponibles", "Exibir Jogos Indisponíveis" },
	{ "Por defecto la biblioteca solo ensena lo que puedes jugar: todo lo que veas, arranca. Con esto puesto salen tambien los que no entran en tu suscripcion, con un candado. Va bien para ojear el catalogo entero, pero hay muchos mas de los que puedes jugar.",
	  "Por padrão a biblioteca só exibe os jogos que você pode jogar. Ao ativar, exibe todo o catálogo do Game Pass, marcando com cadeado os fora do seu plano." },

	{ "Salir de un juego con", "Sair do Jogo com" },
	{ "Que combinacion cierra un juego. Nunca es un boton suelto, y con razon: mientras juegas, cualquier boton es del juego, asi que uno solo te sacaria de la partida cada vez que lo pulsaras. Se mantiene el primero y se pulsa el segundo. Si eliges SELECT + START, desactiva el ajuste de abajo o se pisaran.",
	  "Combinação de botões para fechar a partida em andamento. Segurar SELECT + START por cerca de 300ms também funciona como atalho universal." },

	{ "Cerrar GR33N con START + SELECT", "Fechar GR33N com START + SELECT" },
	{ "El atajo para volver al XMB desde cualquier sitio. Quitalo si te estorba, y quitalo seguro si has puesto SELECT + START para salir de los juegos: si no, salir de una partida te cerraria GR33N entero. Con esto apagado se sale por el XMB, como con cualquier juego.",
	  "Atalho para fechar o aplicativo GR33N e retornar ao menu XMB do PlayStation 3 a qualquer momento." },

	{ "Debug", "Depuração (Debug)" },
	{ "Enseña un panel con fps, estado de la red y rendimiento del decodificador, y manda lo mismo al servidor de depuracion del PC, que guarda cada sesion en un fichero. Tambien anade las pantallas de prueba a la biblioteca. Si algo va mal, esto es lo que deja rastro.",
	  "Exibe um painel com FPS, status da rede e desempenho do decodificador de vídeo, além de salvar detalhes no log da sessão." },

	{ "Tamaño del panel de depuracion", "Tamanho do Painel de Debug" },
	{ "Como de grande sale el panel de estadisticas. A 1 cabe todo pero hay que acercarse a la tele; a 3 se lee desde el sofa pero tapa media pantalla. Solo afecta a ese panel.",
	  "Tamanho visual do painel de estatísticas na tela (1 pequeno, 2 médio, 3 grande)." },

	{ "Esquina del panel de depuracion", "Posição do Painel de Debug" },
	{ "En que esquina se pega el panel de estadisticas. Util cuando lo que quieres mirar esta justo debajo: mueves el panel en vez de la cabeza.",
	  "Em qual canto da tela o painel de estatísticas deve ser fixado." },

	/* Idiomas de juegos (XCLOC) */
	{ "Ingles (EEUU)", "Inglês (EUA)" },
	{ "Ingles (Reino Unido)", "Inglês (Reino Unido)" },
	{ "Espanol (Espana)", "Espanhol (Espanha)" },
	{ "Espanol (Mexico)", "Espanhol (México)" },
	{ "Frances", "Francês" },
	{ "Aleman", "Alemão" },
	{ "Italiano", "Italiano" },
	{ "Portugues (Brasil)", "Português (Brasil)" },
	{ "Neerlandes", "Holandês" },
	{ "Polaco", "Polonês" },
	{ "Ruso", "Russo" },
	{ "Turco", "Turco" },
	{ "Japones", "Japonês" },
	{ "Coreano", "Coreano" },
	{ "Chino simplificado", "Chinês Simplificado" }
};

const char *i18nLookupPt(const char *es)
{
	int i;
	if (!es || es[0] == '\0') return NULL;
	for (i = 0; i < (int)(sizeof(pt_dict)/sizeof(pt_dict[0])); i++) {
		if (strcmp(pt_dict[i].es, es) == 0)
			return pt_dict[i].pt;
	}
	return NULL;
}

void i18nSet(gr33nIdioma i)
{
	if (i >= 0 && i < IDIOMA_N) actual = i;
}

gr33nIdioma i18nGet(void) { return actual; }

const char *i18nCodigo(gr33nIdioma i)
{
	if (i < 0 || i >= IDIOMA_N) return idiomas[IDIOMA_ES].codigo;
	return idiomas[i].codigo;
}

const char *i18nNombre(gr33nIdioma i)
{
	if (i < 0 || i >= IDIOMA_N) return idiomas[IDIOMA_ES].nombre;
	return idiomas[i].nombre;
}

gr33nIdioma i18nPorCodigo(const char *cod)
{
	int i;

	if (cod == NULL || cod[0] == '\0') return IDIOMA_PT;

	for (i = 0; i < IDIOMA_N; i++)
		if (strcmp(idiomas[i].codigo, cod) == 0) return (gr33nIdioma)i;

	return IDIOMA_PT;
}

const char *tr(const char *es, const char *en)
{
	if (actual == IDIOMA_PT) {
		const char *pt = i18nLookupPt(es);
		if (pt) return pt;
	}
	if (actual == IDIOMA_EN && en != NULL && en[0] != '\0') return en;
	return es;
}

/* --------------------------------------------------------------------- */
/* El idioma de xCloud                                                   */
/* --------------------------------------------------------------------- */

static const struct {
	const char *codigo;
	const char *es;
	const char *en;
} xclocs[] = {
#define X(c, e, i) { c, e, i },
	XCLOC_LISTA
#undef X
};

#define XCLOC_TOTAL ((int)(sizeof(xclocs) / sizeof(xclocs[0])))
#define XCLOC_DEFECTO "pt-BR"

static int xcloc_actual = -1;

static int actual_idx(void)
{
	if (xcloc_actual < 0) {
		int i = xclocPorCodigo(XCLOC_DEFECTO);
		xcloc_actual = (i >= 0) ? i : 0;
	}
	return xcloc_actual;
}

int xclocN(void) { return XCLOC_TOTAL; }

const char *xclocCodigo(int i)
{
	if (i < 0 || i >= XCLOC_TOTAL) return "pt-BR";
	return xclocs[i].codigo;
}

const char *xclocNombre(int i)
{
	if (i < 0 || i >= XCLOC_TOTAL) return "?";
	return tr(xclocs[i].es, xclocs[i].en);
}

int xclocPorCodigo(const char *cod)
{
	int i;

	if (cod == NULL || cod[0] == '\0') return -1;

	for (i = 0; i < XCLOC_TOTAL; i++)
		if (strcmp(xclocs[i].codigo, cod) == 0) return i;

	return -1;
}

void xclocSet(int i)
{
	if (i >= 0 && i < XCLOC_TOTAL) xcloc_actual = i;
}

int xclocGet(void) { return actual_idx(); }

const char *xclocActual(void) { return xclocs[actual_idx()].codigo; }
