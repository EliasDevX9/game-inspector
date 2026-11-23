# Game Inspector (mod nativo para LeviLauncher)

Mod para el lado del cliente. Mete un "Cheat Engine" dentro de Minecraft Bedrock (Android, arm64):
busca valores en la memoria del juego, los cambia o los congela, y vigila funciones de `libminecraftpe.so`.

## Compilar
Sube la carpeta a GitHub (por ejemplo `EliasDevX9/game-inspector`). Se compila solo con GitHub Actions
(pestaña Actions → Build → Artifacts → `game_inspector`). Importa `game_inspector.levipack` en LeviLauncher.

## Menú 1: Inspector: escáner de valores
1. Escribe el valor (ej. `1.8`), elige tipo (0=int, 1=float, 2=double) → **PRIMERA BÚSQUEDA**.
2. Cambia algo en el juego (ej. agáchate → ahora vale `1.5`), pon `1.5` en "valor para filtro", filtro 0 → **FILTRAR**.
   También puedes filtrar por: cambió / sin cambio / subió / bajó (sin escribir valor).
3. Repite hasta que queden pocos resultados. El panel de la pantalla muestra la lista (el `>` es el seleccionado).
4. Escribe "Nuevo valor" → **ESCRIBIR** o **CONGELAR** (se reescribe cada 10 ms).
5. **Guardar como botón propio** crea un módulo nuevo (interruptor = congelar) y un botón flotante que aplica el valor.

Valores típicos para empezar: altura del jugador `1.8`, agachado `1.5`, altura de ojos `1.62`, ancho `0.6`.

## Menú 2: Inspector: funciones del juego
- **Buscar**: filtra los símbolos exportados por `libminecraftpe.so` (ej. `sneak`, `Camera`).
  Si el juego no exporta símbolos, usa un offset de Ghidra (`+0x1234ABC`) o una firma de bytes (`48 8B ?? ...`) en "Objetivo manual".
- **VIGILAR**: cuenta cuántas veces se ejecuta y guarda los últimos argumentos (x0..x7, d0). Marca bien qué devuelve (void/int/float/double).
- **RE-EJECUTAR**: la próxima vez que el juego llame la función, la vuelve a ejecutar N veces con los mismos argumentos (dentro del hilo del juego, es lo más seguro).
- **LLAMAR directo**: llama la función con los argumentos que escribas. Avanzado: puede cerrar el juego si necesita un `this` válido.
- **Volcar símbolos**: escribe `simbolos.txt` en la carpeta `data/` del mod, para cruzarlo con Ghidra.

## Límites
- Solo funciones con hasta 8 argumentos enteros/puntero y 8 de coma flotante; no devuelve structs grandes.
- Los resultados del escáner son direcciones de esta sesión (cambian cada vez que abres el juego).
- Probado: compila y la lógica del escáner (buscar/filtrar/escribir/congelar) funciona en un test. No está probado dentro del juego.
