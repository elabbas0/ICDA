void write_string_to_video_memory(const char* str, int color) {
    char* video_memory = (char*) 0xb8000;
    for (int i = 0; str[i] != '\0'; i++) {
        video_memory[i * 2] = str[i];
        video_memory[i * 2 + 1] = color;
    }
}

void kernel_main(){
    write_string_to_video_memory("Our first kernel test", 0x0F);
    while(1);
}

