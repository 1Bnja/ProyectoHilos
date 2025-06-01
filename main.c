#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <string.h>
#include <signal.h>

// Configuración del sistema
#define MAX_PACIENTES 1000
#define MAX_MEDICOS 10
#define MAX_ADMIN 4
#define MAX_ESPECIALISTAS 4

// Factor de velocidad de simulación (1=normal, 2=2x, 4=4x, 10=10x)
int SPEED_FACTOR = 1;

// Tipos de atención
typedef enum {
    ATENCION_GENERAL,
    ATENCION_ENFERMERIA,
    ATENCION_ESPECIALIDAD
} TipoAtencion;

typedef enum {
    CARDIOLOGIA,
    NEUROLOGIA,
    PEDIATRIA,
    DERMATOLOGIA
} Especialidad;

// Estructura del paciente
typedef struct {
    int id;
    TipoAtencion tipo_atencion;
    Especialidad especialidad;
    time_t tiempo_llegada;
    time_t tiempo_clasificacion;
    time_t tiempo_atencion;
    int prioridad; // 1-5, siendo 1 la más alta
    int atendido;
    int abandono;
    int clasificado;
} Paciente;

// Estructura del médico
typedef struct {
    int id;
    TipoAtencion tipo;
    Especialidad especialidad;
    int ocupado;
    int pacientes_atendidos;
    pthread_t thread;
    int activo;
} Medico;

// Estructura del personal administrativo
typedef struct {
    int id;
    int ocupado;
    int pacientes_clasificados;
    pthread_t thread;
    int activo;
} PersonalAdmin;

// Colas del sistema
typedef struct {
    Paciente pacientes[MAX_PACIENTES];
    int frente;
    int final;
    int count;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} Cola;

// Variables globales del sistema
Cola cola_recepcion;
Cola cola_medico_general;
Cola cola_enfermeria;
Cola cola_especialista[4];

PersonalAdmin personal_admin[MAX_ADMIN];
Medico medicos[MAX_MEDICOS];

int num_admin = 2;
int num_medicos_general = 4;
int num_enfermeras = 2;
int num_especialistas = 2;

// Variables para cambio dinámico de personal
int admin_activos = 2;
time_t ultimo_cambio_personal = 0;

// Estadísticas
int total_pacientes_generados = 0;
int total_pacientes_clasificados = 0;
int total_pacientes_atendidos = 0;
int total_pacientes_abandonaron = 0;
int total_no_contabilizados = 0;

pthread_mutex_t stats_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t print_mutex = PTHREAD_MUTEX_INITIALIZER;

volatile int simulacion_activa = 1;
time_t tiempo_inicio_simulacion;

// Manejador de señal para terminar la simulación
void manejador_senal(int sig) {
    (void)sig;
    simulacion_activa = 0;
}

// Función para dormir ajustada por factor de velocidad
void dormir_simulacion(int segundos) {
    if (segundos <= 0) return;
    int tiempo_ajustado = segundos / SPEED_FACTOR;
    if (tiempo_ajustado < 1) {
        usleep((segundos * 1000000) / SPEED_FACTOR); // microsegundos
    } else {
        sleep(tiempo_ajustado);
    }
}

// Funciones de cola
void init_cola(Cola *cola) {
    cola->frente = 0;
    cola->final = 0;
    cola->count = 0;
    pthread_mutex_init(&cola->mutex, NULL);
    pthread_cond_init(&cola->cond, NULL);
}

void enqueue(Cola *cola, Paciente paciente) {
    pthread_mutex_lock(&cola->mutex);
    
    if (cola->count < MAX_PACIENTES) {
        cola->pacientes[cola->final] = paciente;
        cola->final = (cola->final + 1) % MAX_PACIENTES;
        cola->count++;
        pthread_cond_signal(&cola->cond);
    }
    
    pthread_mutex_unlock(&cola->mutex);
}

Paciente dequeue(Cola *cola) {
    pthread_mutex_lock(&cola->mutex);
    
    while (cola->count == 0 && simulacion_activa) {
        pthread_cond_wait(&cola->cond, &cola->mutex);
    }
    
    Paciente paciente = {0};
    if (cola->count > 0) {
        paciente = cola->pacientes[cola->frente];
        cola->frente = (cola->frente + 1) % MAX_PACIENTES;
        cola->count--;
    }
    
    pthread_mutex_unlock(&cola->mutex);
    return paciente;
}

// Insertar con prioridad (prioridad más baja = número menor)
void enqueue_prioridad(Cola *cola, Paciente paciente) {
    pthread_mutex_lock(&cola->mutex);
    
    if (cola->count < MAX_PACIENTES) {
        // Si la cola está vacía, insertar directamente
        if (cola->count == 0) {
            cola->pacientes[cola->final] = paciente;
            cola->final = (cola->final + 1) % MAX_PACIENTES;
            cola->count++;
        } else {
            // Encontrar posición correcta basada en prioridad
            int pos_insercion = cola->final;
            int elementos_movidos = 0;
            
            // Buscar desde el final hacia el frente
            for (int i = 0; i < cola->count; i++) {
                int idx_actual = (cola->final - 1 - i + MAX_PACIENTES) % MAX_PACIENTES;
                if (cola->pacientes[idx_actual].prioridad <= paciente.prioridad) {
                    break;
                }
                // Mover elemento una posición hacia adelante
                int idx_siguiente = (idx_actual + 1) % MAX_PACIENTES;
                cola->pacientes[idx_siguiente] = cola->pacientes[idx_actual];
                pos_insercion = idx_actual;
                elementos_movidos++;
            }
            
            cola->pacientes[pos_insercion] = paciente;
            cola->final = (cola->final + 1) % MAX_PACIENTES;
            cola->count++;
            
            if (elementos_movidos > 0) {
                pthread_mutex_lock(&print_mutex);
                printf("🔄 Paciente %d insertado con prioridad %d, %d pacientes reordenados\n", 
                       paciente.id, paciente.prioridad, elementos_movidos);
                pthread_mutex_unlock(&print_mutex);
            }
        }
        
        pthread_cond_signal(&cola->cond);
    }
    
    pthread_mutex_unlock(&cola->mutex);
}

// Función para verificar abandono basada en tiempo real de espera
int verificar_abandono(time_t tiempo_inicio_espera) {
    time_t tiempo_actual = time(NULL);
    int tiempo_espera_real = tiempo_actual - tiempo_inicio_espera;
    
    // Convertir a tiempo de simulación
    int tiempo_espera_sim = tiempo_espera_real * SPEED_FACTOR;
    
    // Abandonos más frecuentes para mostrar el problema
    // Más de 20 minutos de simulación
    if (tiempo_espera_sim > 1200) {
        return (rand() % 100) < 40; // 40% probabilidad
    }
    // Más de 15 minutos de simulación
    else if (tiempo_espera_sim > 900) {
        return (rand() % 100) < 25; // 25% probabilidad
    }
    // Más de 10 minutos de simulación
    else if (tiempo_espera_sim > 600) {
        return (rand() % 100) < 15; // 15% probabilidad
    }
    
    return 0;
}

// Hilo generador de pacientes
void* generador_pacientes(void* arg) {
    (void)arg;
    srand(time(NULL) + (unsigned long)pthread_self());
    
    while (simulacion_activa) {
        // Tiempo entre llegadas: 5-45 segundos de simulación
        int intervalo = rand() % 41 + 5; // 5-45 segundos
        dormir_simulacion(intervalo);
        
        if (!simulacion_activa) break;
        
        Paciente nuevo_paciente = {0};
        
        pthread_mutex_lock(&stats_mutex);
        nuevo_paciente.id = ++total_pacientes_generados;
        pthread_mutex_unlock(&stats_mutex);
        
        nuevo_paciente.tiempo_llegada = time(NULL);
        nuevo_paciente.prioridad = rand() % 5 + 1;
        
        // Distribución: 70% general, 15% enfermería, 15% especialidad
        int tipo_rand = rand() % 100;
        if (tipo_rand < 70) {
            nuevo_paciente.tipo_atencion = ATENCION_GENERAL;
        } else if (tipo_rand < 85) {
            nuevo_paciente.tipo_atencion = ATENCION_ENFERMERIA;
        } else {
            nuevo_paciente.tipo_atencion = ATENCION_ESPECIALIDAD;
            nuevo_paciente.especialidad = rand() % 4;
        }
        
        enqueue(&cola_recepcion, nuevo_paciente);
        
        pthread_mutex_lock(&print_mutex);
        printf("👤 Paciente %d llegó - Tipo: %s, Prioridad: %d\n", 
               nuevo_paciente.id,
               (nuevo_paciente.tipo_atencion == ATENCION_GENERAL) ? "General" :
               (nuevo_paciente.tipo_atencion == ATENCION_ENFERMERIA) ? "Enfermería" : "Especialidad",
               nuevo_paciente.prioridad);
        pthread_mutex_unlock(&print_mutex);
    }
    
    return NULL;
}

// Hilo del personal administrativo
void* personal_administrativo(void* arg) {
    PersonalAdmin* admin = (PersonalAdmin*)arg;
    srand(time(NULL) + (unsigned long)pthread_self() + admin->id);
    
    while (simulacion_activa) {
        Paciente paciente = dequeue(&cola_recepcion);
        if (paciente.id == 0) continue;
        
        admin->ocupado = 1;
        
        pthread_mutex_lock(&print_mutex);
        printf("📋 Admin %d clasificando paciente %d\n", admin->id, paciente.id);
        pthread_mutex_unlock(&print_mutex);
        
        // Tiempo de clasificación: 2-5 minutos (120-300 segundos)
        int tiempo_clasificacion = rand() % 121 + 60; // 60-180 segundos (1-3 min)
        dormir_simulacion(tiempo_clasificacion);
        
        if (!simulacion_activa) break;
        
        paciente.tiempo_clasificacion = time(NULL);
        paciente.clasificado = 1;
        admin->pacientes_clasificados++;
        
        pthread_mutex_lock(&stats_mutex);
        total_pacientes_clasificados++;
        pthread_mutex_unlock(&stats_mutex);
        
        // Dirigir a la cola correspondiente
        switch (paciente.tipo_atencion) {
            case ATENCION_GENERAL:
                enqueue_prioridad(&cola_medico_general, paciente);
                break;
            case ATENCION_ENFERMERIA:
                enqueue_prioridad(&cola_enfermeria, paciente);
                break;
            case ATENCION_ESPECIALIDAD:
                enqueue_prioridad(&cola_especialista[paciente.especialidad], paciente);
                break;
        }
        
        pthread_mutex_lock(&print_mutex);
        printf("✅ Admin %d clasificó paciente %d hacia %s\n", 
               admin->id, paciente.id,
               (paciente.tipo_atencion == ATENCION_GENERAL) ? "Médico General" :
               (paciente.tipo_atencion == ATENCION_ENFERMERIA) ? "Enfermería" : "Especialista");
        pthread_mutex_unlock(&print_mutex);
        
        admin->ocupado = 0;
    }
    
    return NULL;
}

// Hilo del médico
void* medico_atencion(void* arg) {
    Medico* medico = (Medico*)arg;
    srand(time(NULL) + (unsigned long)pthread_self() + medico->id);
    
    Cola* cola_asignada = NULL;
    const char* tipo_str = "";
    
    switch (medico->tipo) {
        case ATENCION_GENERAL:
            cola_asignada = &cola_medico_general;
            tipo_str = "Médico";
            break;
        case ATENCION_ENFERMERIA:
            cola_asignada = &cola_enfermeria;
            tipo_str = "Enfermera";
            break;
        case ATENCION_ESPECIALIDAD:
            cola_asignada = &cola_especialista[medico->especialidad];
            tipo_str = "Especialista";
            break;
    }
    
    while (simulacion_activa) {
        Paciente paciente = dequeue(cola_asignada);
        if (paciente.id == 0) continue;
        
        // Verificar si el paciente abandonó mientras esperaba
        if (verificar_abandono(paciente.tiempo_clasificacion)) {
            pthread_mutex_lock(&stats_mutex);
            total_pacientes_abandonaron++;
            pthread_mutex_unlock(&stats_mutex);
            
            pthread_mutex_lock(&print_mutex);
            printf("🚪 Paciente %d abandonó la cola por tiempo de espera\n", paciente.id);
            pthread_mutex_unlock(&print_mutex);
            continue;
        }
        
        medico->ocupado = 1;
        paciente.tiempo_atencion = time(NULL);
        
        pthread_mutex_lock(&print_mutex);
        printf("🩺 %s %d atendiendo paciente %d\n", tipo_str, medico->id, paciente.id);
        pthread_mutex_unlock(&print_mutex);
        
        // Tiempo de atención: 12-18 minutos (720-1080 segundos)
        int tiempo_atencion = rand() % 241 + 480; // 480-720 segundos (8-12 min)
        dormir_simulacion(tiempo_atencion);
        
        if (!simulacion_activa) break;
        
        medico->pacientes_atendidos++;
        paciente.atendido = 1;
        
        pthread_mutex_lock(&stats_mutex);
        total_pacientes_atendidos++;
        pthread_mutex_unlock(&stats_mutex);
        
        pthread_mutex_lock(&print_mutex);
        printf("✅ %s %d terminó de atender paciente %d\n", tipo_str, medico->id, paciente.id);
        pthread_mutex_unlock(&print_mutex);
        
        medico->ocupado = 0;
        
        // Pausa entre pacientes: 2-3 minutos (120-180 segundos)
        int tiempo_pausa = rand() % 61 + 60; // 60-120 segundos (1-2 min)
        dormir_simulacion(tiempo_pausa);
    }
    
    return NULL;
}

// Función para gestionar cambios dinámicos de personal
void* gestor_personal(void* arg) {
    (void)arg;
    
    while (simulacion_activa) {
        dormir_simulacion(180); // Cada 3 minutos de simulación
        
        if (!simulacion_activa) break;
        
        time_t ahora = time(NULL);
        if (ahora - ultimo_cambio_personal < 120 / SPEED_FACTOR) continue; // Mín 2 min reales entre cambios
        
        // Evaluar carga del sistema para cambiar personal administrativo
        int carga_recepcion = cola_recepcion.count;
        int nuevo_admin_activos = admin_activos;
        
        if (carga_recepcion > 15 && admin_activos < 4) {
            // Alta carga: activar más personal
            nuevo_admin_activos = (admin_activos < 3) ? admin_activos + 1 : 4;
            
            pthread_mutex_lock(&print_mutex);
            printf("🚨 ALTA DEMANDA: Activando Admin %d (Total activos: %d)\n", 
                   nuevo_admin_activos, nuevo_admin_activos);
            pthread_mutex_unlock(&print_mutex);
            
        } else if (carga_recepcion < 5 && admin_activos > 1) {
            // Baja carga: reducir personal
            nuevo_admin_activos = admin_activos - 1;
            
            pthread_mutex_lock(&print_mutex);
            printf("📉 Baja demanda: Desactivando Admin %d (Total activos: %d)\n", 
                   admin_activos, nuevo_admin_activos);
            pthread_mutex_unlock(&print_mutex);
        }
        
        if (nuevo_admin_activos != admin_activos) {
            admin_activos = nuevo_admin_activos;
            ultimo_cambio_personal = ahora;
        }
        
        // Detectar colapso del sistema
        int total_esperando = cola_recepcion.count + cola_medico_general.count + 
                             cola_enfermeria.count;
        for (int i = 0; i < 4; i++) {
            total_esperando += cola_especialista[i].count;
        }
        
        if (total_esperando > 50) {
            pthread_mutex_lock(&print_mutex);
            printf("🆘 SISTEMA COLAPSADO: %d pacientes esperando en total!\n", total_esperando);
            pthread_mutex_unlock(&print_mutex);
        }
    }
    
    return NULL;
}
void* contador_tiempo(void* arg) {
    (void)arg;
    
    while (simulacion_activa) {
        sleep(10); // Cada 10 segundos reales
        
        if (!simulacion_activa) break;
        
        time_t tiempo_actual = time(NULL);
        int tiempo_real_transcurrido = tiempo_actual - tiempo_inicio_simulacion;
        int tiempo_simulacion_transcurrido = tiempo_real_transcurrido * SPEED_FACTOR;
        
        // Convertir a formato legible
        int horas_real = tiempo_real_transcurrido / 3600;
        int minutos_real = (tiempo_real_transcurrido % 3600) / 60;
        int segundos_real = tiempo_real_transcurrido % 60;
        
        int horas_sim = tiempo_simulacion_transcurrido / 3600;
        int minutos_sim = (tiempo_simulacion_transcurrido % 3600) / 60;
        int segundos_sim = tiempo_simulacion_transcurrido % 60;
        
        pthread_mutex_lock(&print_mutex);
        printf("\n⏰ TIEMPO TRANSCURRIDO:\n");
        printf("   Real: %02d:%02d:%02d | Simulación: %02d:%02d:%02d (x%d)\n", 
               horas_real, minutos_real, segundos_real,
               horas_sim, minutos_sim, segundos_sim, SPEED_FACTOR);
        printf("   Pacientes: Gen=%d, Clas=%d, Atend=%d, Aband=%d\n\n", 
               total_pacientes_generados, total_pacientes_clasificados,
               total_pacientes_atendidos, total_pacientes_abandonaron);
        pthread_mutex_unlock(&print_mutex);
    }
    
    return NULL;
}
void* monitor_sistema(void* arg) {
    (void)arg;
    
    while (simulacion_activa) {
        dormir_simulacion(300); // Cada 5 minutos de simulación
        
        if (!simulacion_activa) break;
        
        pthread_mutex_lock(&print_mutex);
        printf("\n=== ESTADO DEL SISTEMA ===\n");
        printf("Pacientes en recepción: %d\n", cola_recepcion.count);
        printf("Pacientes esperando médico general: %d\n", cola_medico_general.count);
        printf("Pacientes esperando enfermería: %d\n", cola_enfermeria.count);
        
        for (int i = 0; i < 4; i++) {
            const char* especialidades[] = {"Cardiología", "Neurología", "Pediatría", "Dermatología"};
            printf("Pacientes esperando %s: %d\n", especialidades[i], cola_especialista[i].count);
        }
        
        printf("Total generados: %d, Clasificados: %d, Atendidos: %d, Abandonaron: %d\n", 
               total_pacientes_generados, total_pacientes_clasificados, 
               total_pacientes_atendidos, total_pacientes_abandonaron);
        
        printf("Personal administrativo activos: %d/%d - ", admin_activos, num_admin);
        for (int i = 0; i < admin_activos; i++) {
            printf("%s%d", (i > 0) ? ", " : "", personal_admin[i].ocupado);
        }
        printf("\n");
        
        printf("Médicos ocupados: ");
        int total_medicos = num_medicos_general + num_enfermeras + num_especialistas;
        for (int i = 0; i < total_medicos; i++) {
            printf("%s%d", (i > 0) ? ", " : "", medicos[i].ocupado);
        }
        printf("\n");
        
        printf("========================\n\n");
        pthread_mutex_unlock(&print_mutex);
    }
    
    return NULL;
}

// Función para generar reporte final
void generar_reporte() {
    FILE* archivo = fopen("reporte_diario.txt", "w");
    if (!archivo) {
        printf("Error al crear archivo de reporte\n");
        return;
    }
    
    time_t ahora = time(NULL);
    int duracion_real = ahora - tiempo_inicio_simulacion;
    int duracion_simulacion = duracion_real * SPEED_FACTOR;
    
    fprintf(archivo, "REPORTE DIARIO DE ATENCIÓN MÉDICA\n");
    fprintf(archivo, "Fecha: %s", ctime(&ahora));
    fprintf(archivo, "Factor de velocidad utilizado: x%d\n", SPEED_FACTOR);
    fprintf(archivo, "Duración real: %d segundos (%d:%02d:%02d)\n", 
            duracion_real, duracion_real/3600, (duracion_real%3600)/60, duracion_real%60);
    fprintf(archivo, "Duración simulada: %d segundos (%d:%02d:%02d)\n",
            duracion_simulacion, duracion_simulacion/3600, (duracion_simulacion%3600)/60, duracion_simulacion%60);
    fprintf(archivo, "================================\n\n");
    
    fprintf(archivo, "RESUMEN GENERAL:\n");
    fprintf(archivo, "- Pacientes generados: %d\n", total_pacientes_generados);
    fprintf(archivo, "- Pacientes clasificados: %d\n", total_pacientes_clasificados);
    fprintf(archivo, "- Pacientes atendidos: %d\n", total_pacientes_atendidos);
    fprintf(archivo, "- Pacientes que abandonaron: %d\n", total_pacientes_abandonaron);
    
    // Calcular no contabilizados (en recepción)
    total_no_contabilizados = cola_recepcion.count;
    fprintf(archivo, "- Pacientes no contabilizados (en recepción): %d\n", total_no_contabilizados);
    
    // Pacientes sin atención (clasificados pero no atendidos)
    int sin_atencion = (cola_medico_general.count + cola_enfermeria.count);
    for (int i = 0; i < 4; i++) {
        sin_atencion += cola_especialista[i].count;
    }
    fprintf(archivo, "- Pacientes sin atención (clasificados, no atendidos): %d\n\n", sin_atencion);
    
    // Eficiencia del sistema
    if (total_pacientes_generados > 0) {
        float eficiencia = ((float)total_pacientes_atendidos / total_pacientes_generados) * 100;
        fprintf(archivo, "- Eficiencia del sistema: %.2f%%\n\n", eficiencia);
    }
    
    fprintf(archivo, "PERSONAL ADMINISTRATIVO:\n");
    for (int i = 0; i < num_admin; i++) {
        fprintf(archivo, "- Admin %d: %d pacientes clasificados %s\n", 
                personal_admin[i].id, personal_admin[i].pacientes_clasificados,
                (i < admin_activos) ? "(activo)" : "(inactivo al final)");
    }
    
    fprintf(archivo, "\nPERSONAL MÉDICO:\n");
    for (int i = 0; i < num_medicos_general; i++) {
        fprintf(archivo, "- Médico General %d: %d pacientes atendidos\n", 
                medicos[i].id, medicos[i].pacientes_atendidos);
    }
    
    for (int i = 0; i < num_enfermeras; i++) {
        int idx = num_medicos_general + i;
        fprintf(archivo, "- Enfermera %d: %d pacientes atendidos\n", 
                medicos[idx].id, medicos[idx].pacientes_atendidos);
    }
    
    const char* especialidades[] = {"Cardiología", "Neurología", "Pediatría", "Dermatología"};
    for (int i = 0; i < num_especialistas; i++) {
        int idx = num_medicos_general + num_enfermeras + i;
        fprintf(archivo, "- Especialista %s %d: %d pacientes atendidos\n", 
                especialidades[medicos[idx].especialidad], medicos[idx].id, medicos[idx].pacientes_atendidos);
    }
    
    fprintf(archivo, "\nCOLAS AL FINAL DE LA JORNADA:\n");
    fprintf(archivo, "- En recepción: %d pacientes\n", cola_recepcion.count);
    fprintf(archivo, "- Esperando médico general: %d pacientes\n", cola_medico_general.count);
    fprintf(archivo, "- Esperando enfermería: %d pacientes\n", cola_enfermeria.count);
    for (int i = 0; i < 4; i++) {
        fprintf(archivo, "- Esperando %s: %d pacientes\n", especialidades[i], cola_especialista[i].count);
    }
    
    fclose(archivo);
    printf("\n📄 Reporte generado en 'reporte_diario.txt'\n");
}

// Función principal
int main(int argc, char* argv[]) {
    // Parsear factor de velocidad
    if (argc > 1) {
        if (strcmp(argv[1], "x2") == 0) SPEED_FACTOR = 2;
        else if (strcmp(argv[1], "x4") == 0) SPEED_FACTOR = 4;
        else if (strcmp(argv[1], "x10") == 0) SPEED_FACTOR = 10;
        else if (strcmp(argv[1], "x1") == 0) SPEED_FACTOR = 1;
    }
    
    printf("🏥 Iniciando simulación de colas de atención médica (Velocidad: x%d)\n", SPEED_FACTOR);
    printf("Configuración: %d admins (activos: %d), %d médicos generales, %d enfermeras, %d especialistas\n", 
           num_admin, admin_activos, num_medicos_general, num_enfermeras, num_especialistas);
    printf("Presiona Ctrl+C para terminar la simulación\n\n");
    
    // Configurar manejador de señal
    signal(SIGINT, manejador_senal);
    
    // Guardar tiempo de inicio
    tiempo_inicio_simulacion = time(NULL);
    ultimo_cambio_personal = tiempo_inicio_simulacion;
    
    // Inicializar colas
    init_cola(&cola_recepcion);
    init_cola(&cola_medico_general);
    init_cola(&cola_enfermeria);
    for (int i = 0; i < 4; i++) {
        init_cola(&cola_especialista[i]);
    }
    
    // Inicializar personal administrativo
    for (int i = 0; i < num_admin; i++) {
        personal_admin[i].id = i + 1;
        personal_admin[i].ocupado = 0;
        personal_admin[i].pacientes_clasificados = 0;
        personal_admin[i].activo = 1;
        pthread_create(&personal_admin[i].thread, NULL, personal_administrativo, &personal_admin[i]);
    }
    
    // Inicializar médicos generales
    for (int i = 0; i < num_medicos_general; i++) {
        medicos[i].id = i + 1;
        medicos[i].tipo = ATENCION_GENERAL;
        medicos[i].ocupado = 0;
        medicos[i].pacientes_atendidos = 0;
        medicos[i].activo = 1;
        pthread_create(&medicos[i].thread, NULL, medico_atencion, &medicos[i]);
    }
    
    // Inicializar enfermeras
    for (int i = 0; i < num_enfermeras; i++) {
        int idx = num_medicos_general + i;
        medicos[idx].id = i + 1;
        medicos[idx].tipo = ATENCION_ENFERMERIA;
        medicos[idx].ocupado = 0;
        medicos[idx].pacientes_atendidos = 0;
        medicos[idx].activo = 1;
        pthread_create(&medicos[idx].thread, NULL, medico_atencion, &medicos[idx]);
    }
    
    // Inicializar especialistas con especialidades repetidas para mayor rapidez
    srand(time(NULL));
    for (int i = 0; i < num_especialistas; i++) {
        int idx = num_medicos_general + num_enfermeras + i;
        medicos[idx].id = i + 1;
        medicos[idx].tipo = ATENCION_ESPECIALIDAD;
        // Permitir especialidades repetidas (enunciado: "mayor rapidez")
        medicos[idx].especialidad = rand() % 4; // Aleatorio, pueden repetirse
        medicos[idx].ocupado = 0;
        medicos[idx].pacientes_atendidos = 0;
        medicos[idx].activo = 1;
        pthread_create(&medicos[idx].thread, NULL, medico_atencion, &medicos[idx]);
        
        const char* especialidades[] = {"Cardiología", "Neurología", "Pediatría", "Dermatología"};
        printf("Especialista %d: %s\n", medicos[idx].id, especialidades[medicos[idx].especialidad]);
    }
    
    // Crear hilos del sistema
    pthread_t generador_thread, monitor_thread, contador_thread, gestor_thread;
    pthread_create(&generador_thread, NULL, generador_pacientes, NULL);
    pthread_create(&monitor_thread, NULL, monitor_sistema, NULL);
    pthread_create(&contador_thread, NULL, contador_tiempo, NULL);
    pthread_create(&gestor_thread, NULL, gestor_personal, NULL);
    
    // Esperar señal de terminación (Ctrl+C)
    pause();
    
    printf("\n🔄 Terminando simulación...\n");
    simulacion_activa = 0;
    
    // Despertar todos los hilos esperando
    pthread_cond_broadcast(&cola_recepcion.cond);
    pthread_cond_broadcast(&cola_medico_general.cond);
    pthread_cond_broadcast(&cola_enfermeria.cond);
    for (int i = 0; i < 4; i++) {
        pthread_cond_broadcast(&cola_especialista[i].cond);
    }
    
    // Esperar que terminen todos los hilos (con timeout corto)
    printf("Esperando finalización de hilos...\n");
    
    // No esperar indefinidamente por los hilos
    sleep(2);
    
    // Generar reporte final
    time_t tiempo_final = time(NULL);
    int duracion_total_real = tiempo_final - tiempo_inicio_simulacion;
    int duracion_total_sim = duracion_total_real * SPEED_FACTOR;
    
    printf("📊 Duración total - Real: %d segundos | Simulación: %d segundos\n", 
           duracion_total_real, duracion_total_sim);
    
    generar_reporte();
    
    printf("✅ Simulación terminada exitosamente\n");
    
    return 0;
}