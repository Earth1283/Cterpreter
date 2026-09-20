#include <stdio.h>

#define NODES 6
#define INFINITY_COST 1000000

void shortest_paths(int *graph, int start, int *distance, int *parent) {
    int visited[NODES] = {0};
    for (int i = 0; i < NODES; ++i) {
        distance[i] = INFINITY_COST;
        parent[i] = -1;
    }
    distance[start] = 0;

    for (int step = 0; step < NODES; ++step) {
        int nearest = -1;
        for (int i = 0; i < NODES; ++i) {
            if (!visited[i] && (nearest == -1 || distance[i] < distance[nearest])) nearest = i;
        }
        if (nearest == -1 || distance[nearest] == INFINITY_COST) break;
        visited[nearest] = 1;

        for (int next = 0; next < NODES; ++next) {
            int weight = graph[nearest * NODES + next];
            if (weight == 0 || visited[next]) continue;
            int candidate = distance[nearest] + weight;
            if (candidate < distance[next]) {
                distance[next] = candidate;
                parent[next] = nearest;
            }
        }
    }
}

int main(void) {
    int graph[NODES * NODES] = {
         0, 7, 9,  0, 0, 14,
         7, 0,10, 15, 0,  0,
         9,10, 0, 11, 0,  2,
         0,15,11,  0, 6,  0,
         0, 0, 0,  6, 0,  9,
        14, 0, 2,  0, 9,  0
    };
    int distance[NODES];
    int parent[NODES];
    int route[NODES];
    int length = 0;

    shortest_paths(graph, 0, distance, parent);
    for (int i = 0; i < NODES; ++i) printf("0 -> %d: %d\n", i, distance[i]);
    for (int node = 4; node != -1; node = parent[node]) route[length++] = node;
    printf("Route to 4:");
    while (length > 0) printf(" %d", route[--length]);
    putchar('\n');
    return 0;
}
