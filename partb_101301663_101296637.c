#include <stdio.h>
#include <stdlib.h> // for error handling and random number generation
#include <time.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h> // for system calls such as fork()
#include <sys/wait.h> // for process suspension
#include <sys/types.h> // defines process ID type used by wait system call
#include <sys/ipc.h> // defines flags used by shmget
#include <sys/shm.h> // defines system calls which allow loading exams into shared memory
#include <sys/sem.h> // for sempahore system calls and structures

#define RUBRIC_LINES 5 // defines size for the the rubric arrays
#define EXAM_LINES 5 // defines size for the exam answer arrays
#define SHM_KEY 1234 // unique key for shmget
#define SEM_BLOCK 0 // constant for iMayBlock, will suspend process if semaphore is busy

// Union for Initialization
union semun {
    int val;
    struct semid_ds *buf;
    unsigned short *array;
};

// Shared Data Structure:
typedef struct {
    char rubric_answers[RUBRIC_LINES]; // holds correct answers from rubric file
    int student_id; // integer specifying which exam is currently loaded into memory
    char student_answers[EXAM_LINES]; // holds the answers from the exam which is currently loaded into memory
    bool question_marked[EXAM_LINES]; // true if question in current exam has been marked, false otherwise
    int question_remaining; // initialized to 5, when reaching 0 a TA loads the next exam 
    int current_list_index; // tracks what exam we are in the exam list
    bool all_done; // if true, notifies all other TAs and terminates each TA (no offence)
} SharedData;

// Deletes Semaphore:
void SemaphoreRemove(int semid) {
    if (semid != -1) semctl(semid, 0, IPC_RMID, 0);

}

// Allocates and Initializes a Semaphore:
int SemaphoreCreate(int iInitialValue) {
    int semid = semget(IPC_PRIVATE, 1, 0666 | IPC_CREAT); // gets a private semaphore
    if (semid == -1) return semid; // checks if creation failed
    union semun suInitData;
    suInitData.val = iInitialValue;
    // sets value of the semaphore at index 0
    if (semctl(semid, 0, SETVAL, suInitData) == -1) {
        // removes semaphore if failed
        SemaphoreRemove(semid);
        return -1;
    }
    return semid;
}

// Locking Operation:
int SemaphoreWait(int semid, int iMayBlock) {
    struct sembuf sbOperation;
    sbOperation.sem_num = 0; 
    sbOperation.sem_op = -1; // if value is 1, it locks, if 0, it blocks
    sbOperation.sem_flg = iMayBlock;
    return semop(semid, &sbOperation, 1);
}

// Release Operation
int SemaphoreSignal(int semid) {
    struct sembuf sbOperation;
    sbOperation.sem_num = 0;
    sbOperation.sem_op = 1; // releases the lock
    sbOperation.sem_flg = 0;
    return semop(semid, &sbOperation, 1);
}

// Rubric Loader:
void load_rubric(SharedData *data) {
    FILE *fp = fopen("rubric.txt", "r"); // opens rubric in read only
    if (!fp) { 
        perror("Error opening rubric.txt");
        exit(1);
    }
    char line[10]; // buffer holds 1 line of text from rubtic
    int i = 0;

    /*
    The loop iterates RUBRIC_LINES ammount of times, transfers a line from rubric to the line array,
    for each line it return a pointer to the comma in the current line, then sets answer for the rubric to be 
    the character 2 characters after the comma, which corresponds to the correct answer from the rubric file.
    If there is a read error, stops.
    */
    while (fgets(line, sizeof(line), fp) && i < RUBRIC_LINES) {
        char *comma = strchr(line, ',');
        if (comma) data->rubric_answers[i] = *(comma + 2);
        i++;
    }
    fclose(fp);
    printf("System: Rubric Loaded.\n");
}

// Exam Loader:
void load_next_exam(SharedData *data) {
    FILE *list_fp = fopen("exam_list.txt", "r"); // opens a list of exams (e.g, exam_1345, exam_3452, ...)
    if (!list_fp) { 
        perror("Error opening exam_list.txt");
        exit(1);
    }
    char filename[50]; // buffer will hold the file name for the next exam to be loaded
    int current_line = 0;
    bool found = false; // true if entry corresponding to current_list_index is found
    
    /*
    Goes line by line through the exam list until the index of the current_line is reached,
    when the loop reached the correct file, found is set to true which means we have not
    reached the end of the exam list yet, and the while loop ends.
    */
    while (fgets(filename, sizeof(filename), list_fp)) {
        if (current_line == data->current_list_index) {
            filename[strcspn(filename, "\n")] = 0; // uses strcspn to find the index of the newline and replaces it with \0
            found = true;
            break;
        }
        current_line++;
    }
    fclose(list_fp);
    // If there are no more files in the exam list before ID 9999
    if (!found) {
        printf("System: End of exam_list.txt reached. Stopping.\n");
        data->all_done = true;
        return;
    }

    FILE *exam_fp = fopen(filename, "r"); // opens a specific exam from the exam list
    // If there is a problem opening the file we skip that exam
    if (!exam_fp) {
        printf("System: Could not open %s. Skipping.\n", filename);
        data->current_list_index++;
        return;
    }

    char line[10]; // buffer holds 1 line of text from exam file
    // Takes the first line of the exam and converts it to the student ID, if ID = 9999, terminates
    if (fgets(line, sizeof(line), exam_fp)) {
        int id = atoi(line); // converts the first line of the exam file into an integer
        if (id == 9999) {
            printf("System: Student ID 9999 found in %s. Terminating all TAs.\n", filename);
            data->all_done = true;
            fclose(exam_fp);
            return;
        }
        data->student_id = id;
    }

    data->question_remaining = EXAM_LINES; // resets the number of questions left to EXAM_LINES questions
    int i = 0;
    /*
    The loop iterates EXAM_LINES number of times and transfers the exam answer to the line buffer similarly to how
    it was done for the rubric, however this loop also sets each question in the exam to unmarked.
    */
    while (fgets(line, sizeof(line), exam_fp) && i < EXAM_LINES) {
        char *comma = strchr(line, ',');
        if (comma) data->student_answers[i] = *(comma + 2);
        data->question_marked[i] = false;
        i++;
    }
    fclose(exam_fp);
    printf("System: Loaded %s (Student ID: %d). Index: %d\n", filename, data->student_id, data->current_list_index);
    data->current_list_index++;
}

// TA Process:
void ta_process(int ta_id, int rubric_sem, int exam_sem, SharedData *data) {
    srand(getpid()); // uses the TAs pid as a seed to make sure that each TAs behaviour different
    
    while (!data->all_done) {
        for (int i = 0; i < RUBRIC_LINES; i++) {
            usleep(rand() % 500000 + 500000); // TA takes 0.5-1.0 seconds to check rubric answer (smart TA)
            // 5% chance to modify the rubric
            if (rand() % 100 < 5) {
                SemaphoreWait(rubric_sem, 0); // entry section, if 1, decrements to 0 and proceeds, if 0 (another TA editing) blocks

                printf("TA %d: Decided to correct rubric line %d.\n", ta_id, i+1);
                // updates rubric
                char current = data->rubric_answers[i];
                if (current >= 'E') {
                    data->rubric_answers[i] = 'A';
                } else {
                    data->rubric_answers[i] = current + 1;
                }

                SemaphoreSignal(rubric_sem); // exit section, release the lock
            }
        }

        int q_index = -1; // initializes index of the question a TA will mark, -1 means no question selected
        
        // local variable to capture the shared variables during the lock
        int local_student_id;
        char local_student_answer;
        char local_rubric_answer;

        SemaphoreWait(exam_sem, 0); // crated lock for exam data before TA can access questions_marked

        // releases the lock if terminated, prevents deadlock
        if (data->all_done) {
            SemaphoreSignal(exam_sem);
            break;
        }

        for (int j = 0; j < EXAM_LINES; j++) {
            // Checks if a question in the exam is marked yet
            if (!data->question_marked[j]) {
                q_index = j; // TA selects question j+1 to mark
                data->question_marked[j] = true;
                data->question_remaining--;

                // copies shared data to local variables
                local_student_id = data->student_id;
                local_student_answer = data->student_answers[j];
                local_rubric_answer = data->rubric_answers[j];
                break; // question selected, one question at a time
            }
        }

        // checks if load needed during the lock
        bool load_needed = (data->question_remaining <= 0 && !data->all_done);

        // the TA loads exam only if they have the lock
        if (load_needed && q_index == -1) {
             load_next_exam(data);
        }

        SemaphoreSignal(exam_sem); // release exam lock before marking

        if (q_index != -1) {
            // local variable for printing to prevenet printing from another exam if another TA has loaded it
            printf("TA %d: Student %d Q%d: %c (Rubric: %c) -> %s\n",
                ta_id, local_student_id, q_index+1, local_student_answer, local_rubric_answer,
                (local_student_answer == local_rubric_answer ? "Correct" : "Wrong"));
            usleep(rand() % 1000000 + 1000000); // time it takes to grade
        }
    }
    exit(0); // terminates TA process
}

int main(int argc, char *argv[]) {
    // Makes sure the formatting of the command is correct
    if (argc != 2) {
        printf("Usage: %s <number_of_TAs>\n", argv[0]);
        exit(1);
    }
    
    int num_tas = atoi(argv[1]); // converts the selected number of TAs into an int
    
    int shmid = shmget(SHM_KEY, sizeof(SharedData), IPC_CREAT | 0666); // creates shared memory segment based of SharedData, and allows r/w
    if (shmid < 0) {
        perror("shmget");
        exit(1);
    }

    SharedData *data = (SharedData *) shmat(shmid, NULL, 0); // attaches memory segment to parent's address space, all children will have access to this segment
    if (data == (void *)-1) {
        perror("shmat");
        exit(1);
    }

    // creates 2 unlocked binary semaphores for the rubric and exam critical sections
    int rubric_sem = SemaphoreCreate(1);
    int exam_sem = SemaphoreCreate(1);

    if (rubric_sem == -1 || exam_sem == -1) {
        perror("Semaphore creation failed");
        exit(1);
    }

    load_rubric(data); // calls the helper function to load rubric.txt
    data->current_list_index = 0; // sets the next exam to be selected from exam list to the top of the list
    data->all_done = false;
    load_next_exam(data); // loads the first exam into shared memory using helper function

    // duplicates current process num_tas times, and creates ta_process if pid == 0
    for (int i = 0; i <num_tas; i++) {
        if (fork() == 0) {
            ta_process(i + 1, rubric_sem, exam_sem, data);
        }
    }

    // parent process waits until all TAs have terminated
    for (int i = 0; i < num_tas; i++) {
        wait(NULL);
    }
    
    // marks both semaphores for deletion
    SemaphoreRemove(rubric_sem);
    SemaphoreRemove(exam_sem);

    shmdt(data); // detaches the shared memory segment from parent process's address space
    shmctl(shmid, IPC_RMID, NULL); // marks the shared memory segment for destruction

    return 0;
}