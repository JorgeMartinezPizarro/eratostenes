# eratostenes

Criba de Eratostenes segmentada, paralela y empaquetada en bits, sobre una
rueda modulo 2*3*5*7*11 = 2310, pensada para generar listados de primos muy
grandes (probado hasta 10^11 = 100 000 millones) en un tiempo razonable.

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
es previa a la rueda modulo 2310 (ver "Comparativa" mas abajo para el efecto
de ese cambio); a esta escala el cuello de botella real es la escritura a
disco, no el marcado de bits.

## Comparativa: rueda modulo 2310 frente a solo-impares

Medido con `--count-only` (sin E/S, ver mas abajo) para aislar el trabajo de
la CPU, mismo hardware, N=10^10 (pi(10^10) = 455 052 511):

| hilos | solo-impares | rueda mod 2310 | mejora |
|------:|-------------:|----------------:|-------:|
| 1     | 13.01s       | 8.02s            | 1.62x  |
| 12    | 2.00s        | 1.52s            | 1.32x  |

La rueda descarta de entrada los multiplos de 2, 3, 5, 7 y 11 (480 de cada
2310 numeros sobreviven, frente a 15 de cada 30 en la version "solo
impares"), lo que reduce a mas de un tercio el numero de veces que hay que
marcar un compuesto. La mejora medida es menor que esa reduccion teorica
porque cada primo base ahora carga una tabla de saltos de 480 entradas (ver
"Diseno"), y esa tabla -- aunque de solo lectura y compartida entre hilos --
es varios MB por hilo activo y compite por ancho de banda de memoria/cache
L3 cuando muchos hilos la recorren a la vez; por eso la mejora cae de 1.62x
con 1 hilo a 1.32x con 12. Se probo primero una rueda modulo 30 (solo 2, 3 y
5) con una tabla de 8 entradas por primo, y tambien modulo 2310: la tabla
mas grande de esta ultima solo compensa si se guarda en `uint32_t` en vez de
`uint64_t` (ver commits); sin esa reduccion de memoria, la version modulo
2310 llegaba a ser *mas lenta* que la version solo-impares con 12 hilos.

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
  -s, --segment-width N  Ancho numerico de cada segmento (default: 262144)
  -c, --count-only       Solo cuenta los primos, sin escribir el fichero
  -h, --help             Ayuda
```

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
disco distorsione la medida.

## Diseno

**Rueda modulo 2310, no solo impares.** 2310 = 2*3*5*7*11; de cada 2310
numeros consecutivos solo 480 (= phi(2310)) son coprimos con esos cinco
primos, y son los unicos candidatos a primo que se representan. Se numeran
con un "indice de rueda" k=0,1,2,... continuo (`wheel_number`/`wheel_index`
en `src/wheel.hpp`) y cada segmento se representa como un array de bits
(`uint64_t` words) donde el bit i corresponde al numero
`wheel_number(k_low + i)`. 2, 3, 5, 7 y 11 quedan fuera de esa numeracion y
se emiten aparte (caso especial en el hilo 0), igual que el 2 en un sieve
"solo impares". Marcar los multiplos de un primo base p sin dividir en el
bucle caliente requiere, para cada p, una tabla de 480 "saltos" de indice de
rueda precalculada una vez (`compute_wheel_deltas`); esa tabla se guarda en
`uint32_t` (no `uint64_t`) porque su tamano total (varios MB, compartidos
entre hilos) resulto ser el factor dominante para el rendimiento en
paralelo -- ver "Comparativa" mas arriba. Los primos se extraen invirtiendo
cada palabra y recorriendo los bits puestos a 1 con `__builtin_ctzll` +
"clear lowest set bit", en vez de comprobar bit a bit.

**Criba fragmentada (segmentada).** Se calculan primero los primos base
(<= sqrt(N)) con una criba simple en memoria (siempre pequena: sqrt(10^11)
~= 316 228). Despues, el rango de indices de rueda equivalente a [3, N] se
recorre en segmentos pequenos (por defecto equivalentes a ~256K numeros,
configurable con `-s`) que caben en cache L1/L2, marcando multiplos de cada
primo base dentro de cada segmento.

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

- Extender la rueda mas alla de 2310 (p.ej. incluyendo el 13, modulo 30030)
  reduciria aun mas el numero de marcados, pero la tabla de saltos por primo
  creceria proporcionalmente (30030/2310 = 13x mas entradas); dado que ya a
  480 entradas la memoria compartida es el factor limitante en paralelo (ver
  "Comparativa"), es dudoso que compense sin cambiar tambien la estructura
  de la tabla (p.ej. compartirla entre primos con el mismo resto modulo la
  rueda, en vez de una tabla completa por primo).
- El offset de escritura de cada hilo se basa en `pwrite`, especifico de
  POSIX. Para un build nativo de Windows habria que sustituirlo por
  `WriteFile` con `OVERLAPPED` (offset explicito) o volver a un esquema de
  ficheros temporales + fusion.
- No se comprueba espacio libre en disco antes de empezar: para N=100b hacen
  falta unos 49 GB libres en el destino.

## Verificacion

Se comprobo que el conteo de primos coincide con pi(N) conocido para
N = 10, 100, 1000, 10^6, 2*10^6, 10^9, 10^10 y 10^11, incluyendo los limites
alrededor de los primos propios de la rueda (11, 12, 13, 14), y que el
resultado es identico (mismo hash) al variar el numero de hilos (1, 3, 7,
12) y entre `--count-only` y la escritura normal, lo que descarta errores en
los limites entre fragmentos. El fichero de salida de esta version (rueda
modulo 2310) es byte a byte identico al de la version anterior "solo
impares" para los mismos N.
