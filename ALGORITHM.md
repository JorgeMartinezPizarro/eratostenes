## Como funciona eratostenes

Eratostenes funciona con un sistema de 5 pasos, aplicados para conseguir una criba lo mas eficiente posible:

1. Segmentación — problema: memoria. Una criba plana necesita O(N) bits; para N grande eso no cabe en RAM. Se trocea [1,N] en ventanas fijas y pequeñas, reutilizando el mismo array; solo hace falta conocer de antemano los primos ≤ sqrt(N) (base_sieve.hpp, criba plana normal sobre un rango ya pequeño). Cada primo base guarda su "dónde iba" (k, fase) entre ventanas — O(1) por primo, no O(rango). → segment_sieve.hpp.

2. Rueda (wheel) — problema: candidatos redundantes. De cada 30 números, solo 8 son coprimos con 2·3·5 — los otros 22 son múltiplos triviales de 2, 3 o 5. Representar solo esos 8 restos por bloque de 30 reduce el array un ~73% de entrada, antes incluso de sieving. Tamaño fijo en compilación (WHEEL_MOD=30) porque subirlo (mod 210, 2310) hace crecer las tablas por primo más rápido de lo que ahorra en marcado. → wheel.hpp.

3. Paralelismo — problema: un solo hilo es lento. El rango completo se parte en muchos más chunks que hilos (CHUNKS_PER_THREAD=16), repartidos por una cola compartida (no un chunk fijo por hilo), porque el trabajo no es uniforme: los chunks cercanos al final del rango tienen muchos más primos activos por segmento que los del principio. Dos pasadas para el modo texto (cuenta bytes, luego escribe) para que cada hilo escriba en paralelo sin solaparse ni fusionar después; el modo .db evita hasta esa duplicación con un único hilo escritor drenando una cola. → main.cpp::run_parallel_chunks.

4. Presieve — problema: los primos más pequeños golpean cada segmento, sin excepción. El "cubo disperso" (ítem 5) no ayuda con 7, 11, 13... porque su periodo es mucho menor que el segmento — siempre tienen un golpe dentro. En vez de marcarlos cada vez, se precalcula el patrón periódico de varios grupos pequeños de primos (7,23,37 / 11,19,31 / ... hasta 163) y se combina con OR al rellenar el segmento — una copia en bloque en vez de un bucle de marcado por primo. El coste es por grupo (fijo), no por tabla, así que agrupar bien importa más que cubrir más primos (visto hoy: extender más allá de 163 no compensa en esta máquina). → presieve.hpp.

5. Caché — eje transversal, no una fase más. Dos parámetros se autoajustan al hardware real detectado (/sys/.../cache), no a un valor fijo:
- Ancho de segmento ≤ mitad de L2 detectada, para que el array de trabajo de cada hilo quepa en caché incluso compartiendo L2 con su hilo hermano (HT) — confirmado hoy que quitar ese tope empeora, no mejora.
- Presupuesto de tabla de primos base densos ≤ mitad de L3 detectada, para que esa tabla compartida entre hilos siga siendo cache-resident.

