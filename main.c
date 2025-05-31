#include <stdio.h>
#include <stdlib.h>
#include <time.h>

int main() {
    
    srand(time(NULL));
    
    int dado;
    char continuar;
    
    printf("=== SIMULADOR DE DADO ===\n\n");
    
    do {
        
        dado = (rand() % 6) + 1;
        printf("🎲 El dado muestra: %d\n", dado);
        
        printf("\n¿Quieres tirar el dado otra vez? (s/n): ");
        scanf(" %c", &continuar);
        printf("\n");
        
    } while (continuar == 's' || continuar == 'S');
    
    printf("¡Gracias por jugar!\n");
    
    return 0;
}