# Benchmark: rueda optima por N

Tabla de referencia para **Intel Core i5-11400F** (6 nucleos / 12 hilos logicos,
L3 = 12 MB), midiendo `pi(N)` con `--count-only` (sin escritura a disco, para
aislar el trabajo de la CPU/cache de la E/S). Todas las medidas usan la
version de rueda fijada en tiempo de compilacion (`WHEEL_PRIMES` en
`src/wheel.hpp`) -- ver ese fichero para el porque.

Metodologia: para cada N se probaron las 4 ruedas candidatas (`2,3` / `2,3,5`
/ `2,3,5,7` / `2,3,5,7,11`), cambiando `WHEEL_PRIMES` y recompilando
(`make clean && make`) entre cada una. `pi(N)` coincidio exactamente con el
valor conocido en todos los casos.

**Nota**: la tabla de esta seccion se midio con el motor de marcado *previo*
al bucket sieve (barrido de todos los primos activos en cada segmento). El
motor actual usa bucket sieve (ver seccion propia mas abajo) y es mas rapido
para las cuatro ruedas, pero el orden relativo entre ruedas -- que es lo
interesante de esta tabla -- no cambia salvo donde se indica explicitamente.

## Resultados por rueda (todas las N probadas, motor pre-bucket-sieve)

| N | mod 6 | mod 30 | mod 210 | mod 2310 | ganador |
|---|---:|---:|---:|---:|---|
| 10^10 | 1.51s | 1.50s | **1.01s** | 1.52s | mod 210 |
| 10^11 | 21.01s | 18.01s | **16.51s** | 35.56s | mod 210 |
| 10^12 | 352.10s | **303.10s** | 571.78s | 970.67s | mod 30 |
| 10^13 | — | *(pendiente, se probara en el servidor i5-13500)* | — | — | *(pendiente)* |

pi(10^10) = 455 052 511, pi(10^11) = 4 118 054 813, pi(10^12) = 37 607 912 018
-- los tres coinciden con el valor de referencia conocido en las 4 ruedas
probadas para cada N (confirma que cambiar de rueda no cambia el resultado,
solo la velocidad).

## Por que cambia el ganador: tamano de tabla vs L3

Cada primo base necesita una tabla de saltos de `phi(rueda)` entradas
(`uint32_t`, 4 bytes cada una) para marcar sus multiplos sin dividir en el
bucle caliente. Esa tabla, multiplicada por el numero de primos base
(`pi(sqrt(N))`), es la que tiene que caber en la L3 compartida entre los 12
hilos para que el cuello de botella sea CPU y no ancho de banda de memoria:

| N | pi(sqrt(N)) | mod 6 | mod 30 | mod 210 | mod 2310 |
|---|---:|---:|---:|---:|---:|
| 10^10 | 9 592 | 154KB | 384KB | 1.9MB | 18.5MB |
| 10^11 | 27 184 | 435KB | 1.1MB | 5.4MB | 52.4MB |
| 10^12 | 78 498 | 1.3MB | 3.1MB | 15.7MB | 151MB |
| 10^13 | ~224 000 | 3.6MB | ~9.0MB | ~44.8MB | ~431MB |

El punto de cruce entre "mod 210 gana" y "mod 30 gana" cae justo entre 10^11
y 10^12 -- exactamente donde la tabla de mod 210 pasa de 5.4MB (cabe con
margen) a 15.7MB (se sale un 31% de la L3). A 10^13, mod 30 deberia seguir
cabiendo (~9MB, 75% de la L3), pero ya sin tanto margen como a 10^12 (26%);
de ahi que tambien se dejara corriendo mod 6 como comprobacion, por si el
margen menor hace que la mayor densidad de mod 6 (menos tabla, mas marcado)
compense en este punto.

## Bucket sieve: ¿favorece a las ruedas grandes?

El motor pre-bucket recorria **todos** los primos base activos en cada
segmento, marcaran algo o no -- para un primo grande (paso comparable o
mayor que el segmento) la mayoria de esas visitas no marcaban nada, y aun
asi se pagaba el coste de comprobarlo. El bucket sieve programa cada primo
en el "cubo" del segmento futuro donde le toca su proximo multiplo, asi que
cada segmento solo procesa los primos que de verdad tienen trabajo ahi.

Hipotesis antes de medir: como esto elimina el coste de "visitar sin
marcar" -- que penalizaba mas a las ruedas grandes, con filas de saltos mas
grandes que traer para cada visita -- quizas las ruedas grandes (mod 2310)
volverian a ser competitivas a N donde antes perdian por goleada. Medido en
N=10^12 (mismo hardware, 12 hilos):

| rueda | sin bucket sieve | con bucket sieve | mejora |
|---|---:|---:|---:|
| mod 30 | 303.10s | **275.27s** | 1.10x |
| mod 2310 | 970.67s | 599.99s | 1.62x |

**La hipotesis era parcialmente incorrecta.** mod 2310 mejora bastante mas
en terminos relativos (1.62x frente a 1.10x) -- confirma que el bucket
sieve ayuda mas a las ruedas grandes, como se esperaba. Pero mod 30 sigue
ganando claramente en terminos absolutos: mod 2310 pasa de ir 3.20x mas
lento a 2.18x mas lento, cierra hueco pero no le da la vuelta al marcador.

El motivo: el bucket sieve elimina las visitas *sin* trabajo, pero no
reduce el tamano de la fila que hay que traer cuando *si* toca visitar a un
primo -- esa fila mide `WHEEL_SIZE` entradas siempre (480 para mod 2310,
~30 lineas de cache, frente a 8 para mod 30, un unico cache-line), sin
importar si esa visita concreta va a dar 1 acierto o 10. La asimetria
original (anadir un primo a la rueda multiplica la tabla por `(p-1)` pero
solo reduce el trabajo por `(p-1)/p`) sigue intacta: el bucket sieve quita
un coste que era mas o menos independiente de la rueda (visitas vacias),
no el que crece con ella (tamano de fila por visita util). Conclusion:
**sigue siendo mejor una rueda pequena**, con o sin bucket sieve.

## Ancho de segmento: la mejora mas grande, y la mas simple

Tras el bucket sieve, `--segment-width` (`-s`) seguia en su valor heredado
de la version original (`262144`), sin re-evaluar si seguia siendo bueno
para el motor nuevo. No lo era. Cada llamada a `sieve_and_emit` paga un
coste mas o menos fijo por segmento (mirar el cubo, comprobar el puntero de
activacion, preparar la extraccion) independiente de lo ancho que sea ese
segmento -- asi que un segmento mas ancho reparte ese coste fijo entre mas
trabajo util, mientras el array de bits de cada hilo siga cabiendo en su
cache. Barrido completo en N=10^11 (mod 30, bucket sieve, 12 hilos):

| `-s` | tiempo | | `-s` | tiempo |
|---:|---:|---|---:|---:|
| 16 384 | 43.58s | | 2 097 152 | 8.01s |
| 32 768 | 35.56s | | 3 145 728 | 8.01s |
| 65 536 | 29.55s | | **4 194 304** | **7.51s** |
| 131 072 | 23.04s | | 5 242 880 | 7.51s |
| 262 144 (viejo default) | 16.53s | | 6 291 456 | 7.51s |
| 524 288 | 12.52s | | 8 388 608 | 8.01s |
| 1 048 576 | 9.51s | | 16 777 216 | 9.01s |
| | | | 33 554 432 | 30.04s |
| | | | 67 108 864 | 165.75s |

Meseta plana entre 4 194 304 y 6 291 456 (7.51s), y a partir de ~8 388 608
empieza a degradar, con una caida en picado a partir de 33 554 432 -- ahi el
array de bits por hilo (proporcional a `-s`) ya no cabe en L2/L3 y se paga
el mismo problema de ancho de banda de memoria que con una rueda demasiado
grande. `4194304` (`1<<22`) es ahora el nuevo default en
`src/arg_parser.hpp`.

El efecto crece con N (mas segmentos totales = mas coste fijo que
amortizar), igual que el bucket sieve para ruedas limitadas por cache, pero
aqui aplica a **cualquier** rueda:

| N | `-s` viejo (262144) | `-s` nuevo (4194304) | mejora |
|---|---:|---:|---:|
| 10^10 | 1.00s | 1.00s | ~ninguna (pocos segmentos en total) |
| 10^11 | 16.53s | **7.51s** | 2.20x |
| 10^12 | 275.27s | **89.54s** | 3.07x |

## Estado actual acumulado (mod 30, bucket sieve, rama sin branch, `-s`=4194304)

De la primera version de esta sesion (rueda solo-impares, sin bucket sieve,
`-s` sin afinar) a la actual, N=10^12, mismo hardware:

| version | tiempo | factor acumulado |
|---|---:|---:|
| mod 2310 (primera version, ruedas "obvias") | 970.67s | 1.00x |
| mod 30 (rueda correcta para este N) | 303.10s | 3.20x |
| + bucket sieve | 275.27s | 3.53x |
| + `-s` afinado a 4194304 | **89.54s** | **10.84x** |

Con esto, el resultado en el servidor Intel i5-13500 (20 hilos) que ya
batia a gordonBGood (211.06s con la version pre-`-s`-afinado) deberia bajar
considerablemente mas -- pendiente de confirmar con este binario.

## Referencia externa

En un servidor Intel i5-13500 (14 nucleos / 20 hilos, L3 mayor), con
`WHEEL_PRIMES = {2,3,5}` (mod 30), N=10^12 se conto en **211.06s**, superando
una implementacion de referencia en JavaScript de gordonBGood (~402s en el
mismo hardware). Con `{2,3,5,7,11}` (mod 2310, la rueda "obvia" de mas
primos = menos candidatos) el mismo hardware tardaba 673.36s -- mas lento
que la version en JS, hasta que se entendio que el problema no era el
algoritmo de marcado sino el tamano de la tabla frente a la cache.

## Como reproducir

```
# Editar src/wheel.hpp: descomentar la rueda deseada (linea WHEEL_PRIMES), comentar las demas
make clean && make
./eratostenes -n 10000000000 -t 12 --count-only   # 1e10
./eratostenes -n 100000000000 -t 12 --count-only  # 1e11
./eratostenes -n 1t -t 12 --count-only             # 1e12
./eratostenes -n 10000000000000 -t 12 --count-only # 1e13
```

`-s` ya no hace falta pasarlo para reproducir los mejores tiempos: el
default (`4194304`) ya es el optimo medido en esta maquina. Las cifras de
las tablas de rueda mas arriba (secciones "Resultados por rueda" y "Bucket
sieve") se midieron *antes* de afinar `-s`, con el viejo default (`262144`)
-- ver la seccion de ancho de segmento para el efecto de ese cambio por
separado.

Usa siempre `--count-only` a partir de 10^10: el fichero de texto
equivalente ya pesa varios GB en 10^11 y cientos de GB en 10^12+.
