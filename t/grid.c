/* La aritmetica de la rejilla, que es donde viven los fallos de indices.
 * Copiada literalmente de uiUpdate. */
#include <stdio.h>
#define GRID_COLS 7
#define GRID_ROWS 3
static int fails = 0;
#define CHECK(c,...) do{ if(!(c)){printf("FALLO: ");printf(__VA_ARGS__);printf("\n");fails++;} }while(0)

static int sel, top;

static void move(int n, int left, int right, int up, int down)
{
	if (left  && sel > 0) sel--;
	if (right && sel < n - 1) sel++;
	if (up    && sel >= GRID_COLS) sel -= GRID_COLS;
	if (down  && n - sel > GRID_COLS) sel += GRID_COLS;
	{
		int row = sel / GRID_COLS;
		if (row < top) top = row;
		if (row - top >= GRID_ROWS) top = row - GRID_ROWS + 1;
		if (top < 0) top = 0;
	}
}
static int visible(int n) { int f = top*GRID_COLS; int v = n - f; return v > GRID_COLS*GRID_ROWS ? GRID_COLS*GRID_ROWS : v; }

int main(void)
{
	int n, i;

	/* nunca se sale por ningun borde, con cualquier tamano de lista */
	for (n = 1; n <= 600; n++) {
		sel = 0; top = 0;
		for (i = 0; i < 4000; i++) {
			int d = (i * 2654435761u) >> 29;   /* pseudoaleatorio barato */
			move(n, d==0, d==1, d==2, d==3);
			if (sel < 0 || sel >= n) { CHECK(0, "n=%d sel=%d fuera", n, sel); break; }
			if (top < 0) { CHECK(0, "n=%d top negativo", n); break; }
			if (sel/GRID_COLS < top) { CHECK(0, "n=%d seleccion por encima", n); break; }
			if (sel/GRID_COLS - top >= GRID_ROWS) { CHECK(0, "n=%d seleccion por debajo", n); break; }
		}
	}

	/* bajar desde la ultima fila incompleta no debe saltar al vacio */
	n = 16; sel = 14; top = 0;   /* fila 2, col 0; hay 16 (filas 0,1,2 con 2) */
	move(n, 0,0,0,1);
	CHECK(sel == 14, "abajo en la ultima fila se queda: sel=%d", sel);

	n = 10; sel = 3; top = 0;
	move(n, 0,0,0,1);
	CHECK(sel == 10-7 || sel == 3, "n=10 desde 3 abajo -> %d", sel);
	CHECK(sel < n, "sigue dentro");

	/* subir desde la primera fila se queda */
	n = 100; sel = 4; top = 0;
	move(n, 0,0,1,0);
	CHECK(sel == 4, "arriba en la primera fila se queda: %d", sel);

	/* recorrer 586 hasta el final con derecha */
	n = 586; sel = 0; top = 0;
	for (i = 0; i < 700; i++) move(n, 0,1,0,0);
	CHECK(sel == n-1, "derecha hasta el final: %d de %d", sel, n-1);
	CHECK(visible(n) > 0, "queda algo visible al final: %d", visible(n));
	CHECK(sel/GRID_COLS - top < GRID_ROWS, "el ultimo se ve");

	printf(fails ? "%d FALLOS\n" : "rejilla: todo correcto (%d fallos)\n", fails);
	return fails ? 1 : 0;
}
