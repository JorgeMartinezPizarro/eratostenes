# eratostenes

Criba de Eratostenes segmentada, paralela y empaquetada en bits, sobre una
rueda de primos (`WHEEL_PRIMES` en `src/wheel.hpp`, configurable en tiempo
de compilacion) y con marcado por bucket sieve, pensada para generar
listados de primos muy grandes (probado hasta 10^12, en curso hasta 10^13)
en un tiempo razonable.

## Resultado de referencia

```
$ ./eratostenes --limit 100b -o primos_100b.txt -t 12
Listo. 4118054813 primos encontrados hasta 100000000000 (48.90 GB).
  conteo:     23.52s
  escritura:  238.32s
  total:      261.84s (15.7 millones de primos/seg)
```

Medido en un Intel Core i5-11400F (12 hilos logicos), sobre WSL2/ext4, disco
con ~300 MB/s de escritura secuencial sostenida. El resultado (4 118 054 813
primos) coincide exactamente con el valor conocido de pi(10^11). Esta medida
es previa a la introduccion de la rueda de primos (ver "La rueda: cuanto
cache usa" mas abajo); a esta escala el cuello de botella real es la
escritura a disco, no el marcado de bits.

## La rueda: cuanto cache usa, no solo cuantos numeros descarta

Descartar de entrada los multiplos de un conjunto de primos pequenos reduce
el numero de candidatos a comprobar, pero el efecto habitual que se cuenta
(la reduccion asintotica phi(m)/m) es solo la mitad de la historia. Para
marcar multiplos sin dividir en el bucle caliente, cada primo base necesita
una tabla de "saltos" precalculada (`WheelBasePrimes` en `src/wheel.hpp`) de
tamano proporcional a `phi(rueda)`, compartida y releida por todos los
hilos en cada segmento. Anadir un primo p a la rueda multiplica esa tabla
por `(p-1)` pero solo reduce el trabajo de marcado por un factor `(p-1)/p`
-- una asimetria brutal para p grande. En cuanto la tabla deja de caber en
la cache L3, el cuello de botella deja de ser CPU y pasa a ser ancho de
banda de memoria, y el tiempo puede llegar a *empeorar* al anadir mas
primos a la rueda.

Medido con `--count-only` (sin E/S), mismo hardware (i5-11400F, 12 hilos,
L3 = 12 MB), N = 10^12 (pi(10^12) = 37 607 912 018), motor previo al bucket
sieve (ver mas abajo para los tiempos con el motor actual):

| rueda        | phi(mod) | tabla  | vs L3 | tiempo  |
|--------------|---------:|-------:|------:|--------:|
| `2,3`        |        2 |  1.3MB |   11% | 352.10s |
| `2,3,5`      |        8 |  3.1MB |   26% | **303.10s** (mejor) |
| `2,3,5,7`    |       48 | 15.7MB |  131% | 571.78s |
| `2,3,5,7,11` |      480 |   151MB| 1260% | 970.67s |

No es monotono: hay un minimo real en `2,3,5` (mod 30) para este N y esta
cache. Una rueda mas pequena que la optima desperdicia margen de cache
haciendo mas marcado del necesario; una mas grande se sale de la cache y
paga latencia de RAM en casi cada acceso, con retornos decrecientes (pasar
de 131% a 1260% de la L3 solo dobla el tiempo, no lo multiplica por 10).

El punto optimo depende de N (mas N implica mas primos base, tabla mas
grande) y del tamano de L3 de la maquina. Se probo hacer `--wheel` un flag
de CLI (rueda en tiempo de ejecucion) para poder ajustarlo sin recompilar,
y se revirtio: con el divisor de la rueda como variable en vez de constante
de compilacion, GCC deja de poder convertir una division por ese divisor en
un multiply+shift barato, y cada division pasa a ser una division real de
hardware. Eso cuesta ~20% con 1 hilo, pero a N=10^12 con 12 hilos llego a
costar un +44% (303.10s -> 437.13s con la misma rueda `2,3,5`) porque el
numero de esas divisiones crece mas rapido que N. Conclusion: la rueda se
seseleciona editando una linea en `src/wheel.hpp` y recompilando (`make`) --
ver ese fichero para las configuraciones ya preparadas.

Con la rueda correcta (`2,3,5`) en un servidor Intel i5-13500 (20 hilos),
este programa hizo N=10^12 en **211.06s**, superando una implementacion de
referencia en JavaScript de gordonBGood (~402s en el mismo hardware,
confirmado por el usuario) -- con la rueda "obvia" (`2,3,5,7,11`, mas
primos = menos candidatos) el mismo hardware tardaba 673.36s: peor que la
version en JS, hasta que se entendio que el problema no era el algoritmo de
marcado sino el tamano de la tabla frente a la cache.

**Se probo y se descarto** un cursor persistente por primo entre segmentos
(evitar recalcular por division el primer multiplo de cada primo en cada
segmento, arrastrando el estado). En teoria elimina divisiones; en la
practica, a N=10^12 con la rueda de 2310, salio *mas lento* (1102s vs
970.67s): el cuello de botella ya era ancho de banda de memoria para leer
la tabla de saltos, no las divisiones, y el cursor anadio su propio trafico
de memoria por hilo sin atacar la causa real. La leccion (perfilar antes de
optimizar lo que uno *cree* que es el cuello de botella) llevo directamente
al bucket sieve descrito a continuacion.

## Bucket sieve

El motor descrito arriba recorre, en cada segmento, **todos** los primos
base activos -- para un primo grande (paso comparable o mayor que el
segmento) la mayoria de esas visitas no marcan nada, y aun asi se paga el
coste de comprobarlo. El numero de esos pares (primo, segmento) crece mas
rapido que N (aprox. `pi(sqrt(N)) * numero_de_segmentos`, y el numero de
segmentos ya crece linealmente con N), asi que a N grande la mayor parte de
ese recorrido es puro desperdicio.

El bucket sieve invierte el bucle: cada primo se programa en el "cubo" del
segmento futuro donde le toca su proximo multiplo (anillo circular de
tamano fijo, `SegmentSieve` en `src/segment_sieve.hpp`); procesar un
segmento consiste en mirar *solo* su propio cubo, marcar, y reprogramar
cada primo en el cubo que le corresponda a continuacion. Los primos
"nuevos" (cuyo `p*p` acaba de entrar en rango) se activan con un puntero
monotono sobre la lista de primos base (ordenada), una sola vez por primo
en todo el fragmento de un hilo -- no una vez por segmento.

Medido en N=10^12, mismo hardware, comparando el motor de bucket sieve
contra el anterior:

| rueda | sin bucket sieve | con bucket sieve | mejora |
|---|---:|---:|---:|
| `2,3` (mod 6) | 352.10s | 306.90s | 1.15x |
| `2,3,5` (mod 30) | 303.10s | **275.27s** | 1.10x |
| `2,3,5,7,11` (mod 2310) | 970.67s | 599.99s | 1.62x |

La rueda `2,3,5,7,11` mejora mucho mas en terminos relativos (le sobraban
visitas vacias, y esas son justo las que el bucket sieve elimina), pero
sigue perdiendo claramente en terminos absolutos frente a `2,3,5`: pasa de
ir 3.20x mas lenta a 2.18x mas lenta, cierra hueco pero no da la vuelta al
marcador. El motivo: el bucket sieve quita el coste de "visitar sin
marcar", que era mas o menos independiente de la rueda, pero no reduce el
tamano de la fila que hay que traer cuando *si* toca visitar a un primo --
esa fila mide `phi(rueda)` entradas siempre (480 para 2310, ~30 lineas de
cache; 8 para mod 30, una sola linea), sin importar cuantos aciertos vaya a
dar esa visita concreta. La asimetria original (anadir un primo multiplica
la tabla por `(p-1)` pero solo reduce el trabajo por `(p-1)/p`) sigue
intacta. **Sigue siendo mejor una rueda pequena**, con o sin bucket sieve.

El efecto tambien depende de si la rueda estaba limitada por cache o no.
Para `2,3` y `2,3,5` (tablas que siempre cupieron en L3, con o sin bucket
sieve) la mejora es modesta y no crece de forma clara con N -- medido
tambien en 10^10 y 10^11:

| N | mod 6, sin bucket | mod 6, con bucket | mod 30, sin bucket | mod 30, con bucket |
|---|---:|---:|---:|---:|
| 10^10 | 1.51s | 1.50s | 1.50s | **1.01s** |
| 10^11 | 21.01s | 19.02s | 18.01s | **17.53s** |
| 10^12 | 352.10s | 306.90s | 303.10s | **275.27s** |

Nada monotono ahi (1.49x, 1.03x y 1.10x de mejora para mod 30 en 10^10,
10^11 y 10^12 respectivamente). El "mejora con N" que si se observa
claramente es especifico de ruedas limitadas por cache (mod 2310 arriba),
donde el problema que el bucket sieve ataca empeora con N; para mod 6 y mod
30, cuya tabla siempre cupo, nunca hubo ese problema que arreglar a esa
escala.

Otras dos mejoras "simples" que se probaron junto al bucket sieve: quitar
la rama de reinicio de fase (`if (j==WHEEL_SIZE) j=0`) por una mascara AND
cuando `WHEEL_SIZE` es potencia de 2 -- cierto para mod 6 y mod 30, las dos
ruedas que de verdad importan -- y afinar `-s` (ver siguiente seccion, la
que mas dio de las tres con diferencia).

## Ancho de segmento: la mejora mas grande

Tras meter el bucket sieve, `-s` seguia en su valor heredado de la version
original (`262144`) sin reevaluarlo. Cada llamada a `sieve_and_emit` paga
un coste mas o menos fijo por segmento (mirar el cubo, comprobar el puntero
de activacion, preparar la extraccion), independiente de lo ancho que sea
ese segmento -- asi que un segmento mas ancho reparte ese coste entre mas
trabajo util, mientras el array de bits de cada hilo (proporcional a `-s`)
siga cabiendo en su cache. Medido en N=10^12 (mod 30, bucket sieve, 12
hilos), barriendo `-s`: mejora monotona hasta `4194304` (**89.54s**, 3.07x
mas rapido que con el default viejo), meseta hasta `6291456`, y despues
degrada con fuerza -- caida en picado a partir de `33554432`, donde el
array de bits ya no cabe en cache. `4194304` (`1<<22`) es ahora el default.
Barrido completo y el mismo efecto en 10^10/10^11 en `benchmark.md`.

**Resultado acumulado, N=10^12, mismo hardware, de la primera version de
este proyecto a la actual:**

| version | tiempo | factor |
|---|---:|---:|
| mod 2310 (rueda "obvia", primera version) | 970.67s | 1.00x |
| mod 30 (rueda correcta para este N) | 303.10s | 3.20x |
| + bucket sieve | 275.27s | 3.53x |
| + `-s` afinado | **89.54s** | **10.84x** |

## Compilar

Requiere un compilador con C++20 y `pwrite`/`ftruncate` de POSIX (Linux o
WSL; no compila tal cual con MSVC/Windows nativo).

```
make            # build de produccion: -O3 -march=native -flto
make portable   # sin -march=native, para llevar el binario a otra maquina
make debug      # con ASan/UBSan, para depurar
```

Si trabajas en Windows con el proyecto en `/mnt/c/...`, compila y ejecuta
**dentro de WSL** apuntando la salida a un directorio nativo de Linux (p.ej.
`~/...`), no a `/mnt/c/...`: ese punto de montaje pasa por 9p y es mucho mas
lento para E/S intensiva. En las pruebas, escribir en `/mnt/c` fue el cuello
de botella real, no la CPU.

## Docker

```
make docker                                              # construye la imagen
make run ARGS="--limit 100b -o /output/primos.txt -t 12" # ejecuta desde la imagen
```

`make run` monta `./output` (host) en `/output` (contenedor) y crea el
directorio si no existe; usa siempre `-o /output/<fichero>` en `ARGS` para
que el resultado quede accesible fuera del contenedor. Sin `ARGS`, se
ejecuta con `--help`.

## Uso

```
./eratostenes --limit N [opciones]

  -n, --limit N          Limite superior (inclusive). Acepta sufijos:
                          k=1e3  m=1e6  b=1e9 (billon ingles)  t=1e12
                          Ej: 100b = 10^11
  -o, --output PATH      Fichero de salida (default: primes.txt)
  -t, --threads N        Numero de hilos (default: nucleos disponibles)
  -s, --segment-width N  Ancho numerico de cada segmento (default: 4194304)
  -c, --count-only       Solo cuenta los primos, sin escribir el fichero
  -h, --help             Ayuda
```

La rueda (que primos se descartan de entrada) se fija en tiempo de
compilacion en `src/wheel.hpp` (`WHEEL_PRIMES`) -- ver "La rueda" arriba
antes de tocarla: mas primos no siempre es mas rapido, depende de N y de la
cache L3 de tu CPU.

Ejemplos:

```
./eratostenes -n 1000000 -o primos_1M.txt
./eratostenes -n 100b -o ~/primos_100b.txt -t 12
./eratostenes -n 100b -t 12 --count-only
```

`--count-only` salta por completo la segunda pasada (no crea ni
redimensiona ningun fichero, ni convierte los primos a texto): solo corre
la pasada de conteo, con un sink que no hace nada (`NullSink`) en vez de
`ByteCounter`, asi que tampoco paga el coste de `to_chars` por cada primo.
Util para medir pi(N) o el rendimiento puro de la criba sin que la E/S a
disco distorsione la medida -- imprescindible a partir de 10^11 o asi,
donde el fichero de texto ya pesa decenas de GB.

## Diseno

**Rueda en tiempo de compilacion, con bucket sieve.** `WHEEL_PRIMES` en
`src/wheel.hpp` fija que primos se descartan de entrada (editar esa linea y
`make` para cambiarla -- ver "La rueda" arriba para por que no es un flag
de CLI); el resto de la rueda (modulo, residuos, tabla de posiciones) se
deriva de ahi, tambien en tiempo de compilacion via `constexpr`. Los
numeros que sobreviven a la rueda se numeran con un "indice de rueda"
k=0,1,2,... continuo (`wheel_number`/`wheel_index`) y cada segmento se
representa como un array de bits (`uint64_t` words) donde el bit i
corresponde al numero `wheel_number(k_low + i)`. Los primos propios de la
rueda quedan fuera de esa numeracion y se emiten aparte (caso especial en
el hilo 0). Marcar los multiplos de un primo base p sin dividir en el bucle
caliente requiere, para cada p, una tabla de saltos de indice de rueda
precalculada una vez (`compute_wheel_deltas`); todas las tablas viven en
**un unico buffer plano y contiguo** (`std::array` dentro de `WheelBasePrime`,
no un `vector` por primo) para conservar el patron de acceso secuencial, y
se guardan en `uint32_t` (no `uint64_t`) para reducir a la mitad su huella
en cache -- ver "La rueda" mas arriba para por que ese tamano es lo que de
verdad determina el rendimiento a partir de cierto N. El marcado en si usa
bucket sieve (ver esa seccion arriba): cada primo se programa en el cubo
del segmento donde le toca su proximo multiplo, en vez de comprobarse en
cada segmento este o no activo ahi. Los primos se extraen invirtiendo cada
palabra y recorriendo los bits puestos a 1 con `__builtin_ctzll` + "clear
lowest set bit", en vez de comprobar bit a bit.

**Criba fragmentada (segmentada).** Se calculan primero los primos base
(<= sqrt(N)) con una criba simple en memoria (siempre pequena: sqrt(10^12)
= 10^6). Despues, el rango de indices de rueda equivalente a [3, N] se
recorre en segmentos (por defecto equivalentes a ~4.19M numeros,
configurable con `-s`) marcando multiplos de cada primo base dentro de cada
segmento. Con el bucket sieve, el ancho ideal ya no es "quepa en L1/L2" --
es un compromiso entre amortizar el coste fijo por segmento (mas ancho,
mejor) y que el array de bits de cada hilo siga cabiendo en su cache (mas
ancho, peor pasado cierto punto): medido en N=10^12, el default viejo
(262144) daba 275.27s, el nuevo (4194304) da **89.54s** (3.07x), y pasado
~8M empieza a degradar, con caida en picado a partir de 33M. Ver
`benchmark.md` para el barrido completo.

**Paralelizacion.** El rango completo se divide en tantos fragmentos
contiguos como hilos, cada uno cribando su propio tramo de forma
independiente (los primos base son de solo lectura, compartidos sin
bloqueos). El trabajo por numero es aproximadamente uniforme, asi que un
reparto estatico en fragmentos iguales ya da buen balance de carga.

**Escritura sin fusion posterior.** La primera version de este proyecto
escribia un fichero temporal por hilo y despues los concatenaba. Eso
duplica la E/S en disco: cada byte del resultado se escribe una vez al
fichero temporal y otra vez al fusionar. Con 45-49 GB de salida, esa segunda
copia fue el cuello de botella (57s de 64s en una prueba con 10^10).

En su lugar, el programa hace dos pasadas:

1. **Conteo** (`ByteCounter`): cada hilo criba su fragmento igual que en la
   pasada final, pero en vez de escribir, solo cuenta cuantos bytes de texto
   ocuparan sus primos. No hay E/S en esta fase.
2. Con esos totales se calcula por prefijos el offset exacto donde debe
   empezar a escribir cada hilo, y se redimensiona el fichero final a su
   tamano definitivo (`ftruncate`/`resize_file`, operacion barata incluso
   para ficheros de decenas de GB).
3. **Escritura** (`DirectWriter`): cada hilo vuelve a cribar su fragmento y
   escribe con `pwrite()` directamente en su region (disjunta) del fichero
   final, en paralelo con el resto.

El coste que se paga es repetir la fase de marcado de bits dos veces (barata,
limitada por CPU/cache: ~24s de 262s en la prueba de 10^11). A cambio se
evita repetir la escritura a disco (cara, limitada por I/O), que es el
recurso mas escaso a esta escala. En la practica esto dio un x2.9 de mejora
frente al esquema con fusion (22s vs 64s cribando hasta 10^10).

## Limitaciones y posibles mejoras futuras

- Para N muy por encima de 10^12 (p.ej. 10^14), incluso la rueda `2,3,5`
  puede dejar de caber en L3 (su tabla escala con pi(sqrt(N)); a 10^14
  serian unos 25MB). En ese punto haria falta bajar a `2,3` o replantear la
  estructura de la tabla (p.ej. compartirla entre primos con el mismo resto
  modulo la rueda, en vez de una fila completa por primo).
- El offset de escritura de cada hilo se basa en `pwrite`, especifico de
  POSIX. Para un build nativo de Windows habria que sustituirlo por
  `WriteFile` con `OVERLAPPED` (offset explicito) o volver a un esquema de
  ficheros temporales + fusion.
- No se comprueba espacio libre en disco antes de empezar: para N=100b hacen
  falta unos 49 GB libres en el destino, y para N=1t, del orden de 500GB
  (usa `--count-only` si solo te interesa pi(N)).

## Verificacion

Se comprobo que el conteo de primos coincide con pi(N) conocido para
N = 10, 100, 1000, 10^6, 2*10^7, 10^8, 5*10^7, 10^9, 10^10, 10^11 y 10^12,
incluyendo los limites alrededor de los primos propios de la rueda (11, 12,
13, 14), y que el resultado es identico (mismo hash) al variar el numero de
hilos (1, 3, 7, 12), entre `--count-only` y la escritura normal, entre
distintos anchos de segmento (`-s` de 512 a 2 millones, lo que ademas
estresa el dimensionamiento del anillo de cubos del bucket sieve), y
**entre distintas ruedas** (`2,3` / `2,3,5` / `2,3,5,7` producen el mismo
fichero byte a byte para el mismo N, recompilando entre cada una) -- lo que
descarta tanto errores en los limites entre fragmentos como errores
especificos de una configuracion de rueda o de un tamano de anillo.
