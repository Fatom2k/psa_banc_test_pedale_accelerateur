# Banc de test pédale d'accélérateur PSA — TTGO T-Display v1.1

Testeur autonome pour la pédale **1261** déposée (hors véhicule). Le module alimente la pédale en 5 V, lit S1, S2 et le 5 V réel, puis affiche la cohérence des deux pistes (S1 ≈ 2 × S2), celle que le calculateur 1320 surveille (P0121/P0122).

## 1. Brochage de la pédale (connecteur 4 voies noir)

| Broche pédale | Rôle | Fil | Calculateur |
|---|---|---|---|
| 1 | S1 (piste 1) | 1377 | F2 |
| 2 | Masse | 1379 | F1 |
| 3 | S2 (piste 2) | 1378 | G2 |
| 4 | +5 V | 1380 | G1 |

Valeurs attendues : S2 de 0,15–0,3 V (relâchée) à 1,75–2,0 V (à fond), S1 environ le double.

## 2. Matériel

- LilyGO TTGO T-Display v1.1
- 6 résistances de 47 kΩ, 1 résistance de 10 kΩ, 1 résistance de 22 Ω
- 3 condensateurs de 100 nF
- Une contre-prise 4 voies (ou des cosses femelles fines) pour la pédale
- *Option précision :* un module ADS1115 (I2C, 16 bits)

## 3. Câblage (version de base, ADC de l'ESP32)

```
 T-Display 5V ──[22 Ω]──┬──────────────────────────► Pédale broche 4 (+5 V)
                        │
                        └─[47k]─[47k]─┬─► GPIO36 (mesure Vcc, ÷3)
                                      ├─[47k]─ GND
                                      └─[100nF]─ GND

 Pédale broche 1 (S1) ──[47k]─┬──────► GPIO33 (÷2)
                              ├─[47k]─ GND
                              └─[100nF]─ GND

 Pédale broche 3 (S2) ─┬─[10k]─┬─────► GPIO32 (÷1)
                       │       └─[100nF]─ GND
                       └─[47k]─ GND        (tirage : détecte une piste coupée)

 Pédale broche 2 (masse) ────────────────► GND T-Display
```

Pourquoi ces valeurs :
- L'ADC de l'ESP32 accepte au plus ~2,5 V et lit mal sous ~0,15 V. S1 (jusqu'à 4 V) est donc divisé par 2. S2 (≤ 2 V) est lu directement pour ne pas tomber dans la zone morte basse.
- Le 5 V est mesuré pour calculer l'écart **en % de Vcc**. Ainsi la mesure reste juste même si l'USB donne 4,8 V au lieu de 5,0 V.
- La 22 Ω et la 10 kΩ protègent la carte en cas de court-circuit dans une pédale abîmée.

> Pour plus de justesse, mesurez vos résistances et corrigez `DIV_S1`, `DIV_S2` et `DIV_VCC` en haut de `main.cpp`.

### Option ADS1115 (recommandée)

L'ADC de l'ESP32 donne une précision de 1 à 2 %, ce qui reste juste face aux seuils de 3 et 6 %. L'ADS1115 est bien plus précis et n'a pas de zone morte près de 0 V.

- ADS1115 : VDD → 3V3, GND → GND, SDA → GPIO21, SCL → GPIO22, ADDR → GND
- A0 ← point milieu du pont S1, A1 ← S2 (après la 10k), A2 ← point milieu du pont Vcc (mêmes ponts que ci-dessus)
- Dans `platformio.ini`, décommenter `-DUSE_ADS1115`

## 4. Compilation

**PlatformIO (recommandé)** : ouvrir le dossier puis lancer *Upload*. L'écran est déjà configuré dans `platformio.ini`, sans toucher à la bibliothèque.

**Arduino IDE** :
1. Installer la carte « esp32 » par Espressif, **version 2.0.17**. TFT_eSPI pose problème avec les versions 3.x.
2. Installer les bibliothèques *TFT_eSPI* (Bodmer) et, en option, *Adafruit ADS1X15*.
3. Dans `libraries/TFT_eSPI/User_Setup_Select.h`, commenter `#include <User_Setup.h>` et décommenter `#include <User_Setups/Setup25_TTGO_T_Display.h>`.
4. Renommer `main.cpp` en `banc_pedale.ino` dans un dossier `banc_pedale/`. Choisir la carte *ESP32 Dev Module* et téléverser.

## 5. Utilisation

- **Bouton GPIO0** : écran suivant. **Bouton GPIO35** : remise à zéro des mesures.
- Mesures : ~400 Hz (ADC interne) ou ~250 Hz (ADS1115), en tâche dédiée. Les pointes brèves ne sont donc pas perdues pendant l'affichage.

| Écran | Contenu |
|---|---|
| MESURE | S1, 2×S2, S2, Vcc, rapport S1/S2, **écart en %**, position, écart max et sa position, compteurs d'incidents et de hors-plage |
| TEMPS 12 s | S1 et 2×S2 superposés (ils doivent se confondre), écart min/max sur ±10 %. Une microcoupure apparaît comme un trait vertical |
| BALAYAGE | Écart max **par position** de la pédale. Révèle une zone morte ou une perturbation à un endroit précis de la course |
| STATISTIQUES | Pente S1/S2 réelle (≈ 2,0), décalage, R², min/max de chaque piste et du 5 V |

Voyant d'état : `OK` / `ECART` (> 3 %) / `INCOHERENT` (> 6 %, compté comme incident) / `S1 HS` / `S2 HS` / `PEDALE ?` / `ALIM 5V ?`.
Ces seuils sont des réglages du banc et non des valeurs constructeur PSA. Ils sont modifiables en haut de `main.cpp`.

## 6. Procédure de test conseillée

1. Brancher et laisser la pédale relâchée. Vérifier S2 entre 0,15 et 0,3 V, S1 ≈ 2×S2 et le statut `OK`.
2. Appuyer sur GPIO35 (RAZ), puis aller sur l'écran **BALAYAGE**.
3. Enfoncer la pédale **très lentement** jusqu'en butée, puis la relâcher lentement, 3 à 4 fois. Toute la largeur doit être couverte (« vu » proche du maximum) et rester verte.
4. Faire des coups rapides (relâché → à fond) en surveillant l'écran **TEMPS**.
5. **Test en température** : chauffer le boîtier du capteur au décapeur (tiède, < 70 °C) ou le refroidir à la bombe de froid, en répétant les balayages. Sur le forum C2, le défaut disparaissait après refroidissement.
6. Tapoter le boîtier et remuer le connecteur pour révéler un faux contact. Le compteur *hors plage* doit rester à 0.
7. Relever l'écran **STATISTIQUES** : la pente doit être ≈ 2,00 et le décalage proche de 0.

Journal : le port série (115200 bauds) envoie en continu `t_ms,S1_V,S2_V,Vcc_V,ecart_pct,ecart_min,ecart_max`. On peut l'ouvrir dans le traceur série de l'Arduino IDE ou l'enregistrer en CSV pour comparer deux pédales.
