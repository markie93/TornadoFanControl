#include <windows.h>
#include <iostream>
#include <thread>
#include <chrono>
#include <vector>
#include <cmath>
#include <atomic>
#include <string>
#include <fstream>
#include <shlobj.h>  // Add this include for SHGetFolderPath

typedef short(__stdcall* InpFunc)(short port);
typedef void(__stdcall* OutFunc)(short port, short data);

InpFunc Inp32;
OutFunc Out32;

// EC Registers
const short EC_DATA_PORT = 0x62;
const short EC_COMMAND_PORT = 0x66;
const short EC_READ_CMD = 0x80;
const short EC_WRITE_CMD = 0x81;

const short gpu_fan_mode_reg = 33;
const short gpu_fan_mode_auto = 0x10;
const short gpu_fan_mode_manual = 0x30;

const short cpu_fan_mode_reg = 34;
const short cpu_fan_mode_auto = 0x04;
const short cpu_fan_mode_manual = 0x0c;

const short fan_mode_reg = 44;
const short fan_mode_full_manual = 0x00;

const short gpu_fan_speed_reg = 58;
const short cpu_fan_speed_reg = 55;

const short cpu_temp_reg_1 = 176;
const short gpu_temp_reg_1 = 180;

struct SharedData 
{
    std::atomic<double> cpuTemp;
    std::atomic<double> gpuTemp;
    std::atomic<int> cpuFanSpeed;
    std::atomic<int> gpuFanSpeed;

    std::atomic<int> cpu_hysteresis;
    std::atomic<int> gpu_hysteresis;

    std::atomic<int> cpu_temp_point_2;
    std::atomic<int> cpu_temp_point_3;
    std::atomic<int> cpu_temp_point_4;

    std::atomic<int> gpu_temp_point_2;
    std::atomic<int> gpu_temp_point_3;
    std::atomic<int> gpu_temp_point_4;

    std::atomic<int> cpu_fan_point_1;
    std::atomic<int> cpu_fan_point_2;
    std::atomic<int> cpu_fan_point_3;
    std::atomic<int> cpu_fan_point_4;
    std::atomic<int> cpu_fan_point_5;
                         
    std::atomic<int> gpu_fan_point_1;
    std::atomic<int> gpu_fan_point_2;
    std::atomic<int> gpu_fan_point_3;
    std::atomic<int> gpu_fan_point_4;
    std::atomic<int> gpu_fan_point_5;

    std::atomic<bool> write_sync;

    // PID mode fields (appended for backward compat)
    std::atomic<int> fan_ctrl_mode;   // 0 = curve, 1 = PID
    std::atomic<int> cpu_target_temp; // PID target CPU temp (°C)
    std::atomic<int> gpu_target_temp; // PID target GPU temp (°C)
    std::atomic<int> cpu_min_fan_pid; // minimum CPU fan % in PID mode
    std::atomic<int> gpu_min_fan_pid; // minimum GPU fan % in PID mode
};

// Global variables for cleanup
std::atomic<bool> g_running(true);
SharedData* g_sharedData = nullptr;
HANDLE g_hMapFile = nullptr;

void wait_for_ibf_clear()
{
    for (int i = 0; i < 100; ++i) {
        if ((Inp32(EC_COMMAND_PORT) & 0x02) == 0)
            return;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    throw std::runtime_error("EC input buffer busy");
}

void ec_write(short reg, short val)
{
    wait_for_ibf_clear();
    Out32(EC_COMMAND_PORT, EC_WRITE_CMD);
    wait_for_ibf_clear();
    Out32(EC_DATA_PORT, reg);
    wait_for_ibf_clear();
    Out32(EC_DATA_PORT, val);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

short ec_read(short reg)
{
    wait_for_ibf_clear();
    Out32(EC_COMMAND_PORT, EC_READ_CMD);
    wait_for_ibf_clear();
    Out32(EC_DATA_PORT, reg);
    wait_for_ibf_clear();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    short temp = Inp32(EC_DATA_PORT);
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    return temp;
}

class LowPassFilter
{
public:
    LowPassFilter(double alpha) : alpha(alpha), initialized(false), filtered(0) {}

    double filter(double value) {
        if (!initialized) {
            filtered = value;
            initialized = true;
        }
        else {
            filtered = alpha * value + (1 - alpha) * filtered;
        }
        return filtered;
    }

private:
    double alpha;
    bool initialized;
    double filtered;
};

class FanController
{
public:
    FanController(std::vector<int> tempSet, std::vector<int> fanSet, int hyst)
        : tempSetpoints(tempSet), fanSetpoints(fanSet), hysteresis(hyst), fanSpeed(0), lastTemp(0) 
    {
        if ((fanSet.size() != tempSetpoints.size()) || (fanSet.size() != 5))
        {
            throw std::invalid_argument("tempSetpoints and fanSetpoints must be 5");
		}
    }

    int update(double temp) 
    {
        if ((lastTemp - temp > hysteresis) || (temp > lastTemp)) {
            for (size_t i = 0; i < tempSetpoints.size() - 1; ++i) {
                if (temp >= tempSetpoints[i] && temp < tempSetpoints[i + 1]) {
                    double temp_range = tempSetpoints[i + 1] - tempSetpoints[i];
                    double fan_range = fanSetpoints[i + 1] - fanSetpoints[i];
                    fanSpeed = static_cast<int>(fanSetpoints[i] + (temp - tempSetpoints[i]) * fan_range / temp_range);
                    break;
                }
            }
            lastTemp = temp;
        }
        return fanSpeed;
    }

    void syncToSharedData(SharedData* sharedData, bool isCpuController)
    {
        if (isCpuController) 
        {
            sharedData->cpu_hysteresis.store(hysteresis);
            sharedData->cpu_temp_point_2.store(tempSetpoints[1]);
            sharedData->cpu_temp_point_3.store(tempSetpoints[2]);
            sharedData->cpu_temp_point_4.store(tempSetpoints[3]);
            sharedData->cpu_fan_point_1.store(fanSetpoints[0]);
            sharedData->cpu_fan_point_2.store(fanSetpoints[1]);
            sharedData->cpu_fan_point_3.store(fanSetpoints[2]);
            sharedData->cpu_fan_point_4.store(fanSetpoints[3]);
            sharedData->cpu_fan_point_5.store(fanSetpoints[4]);
        } 
        else 
        {
            sharedData->gpu_hysteresis.store(hysteresis);
            sharedData->gpu_temp_point_2.store(tempSetpoints[1]);
            sharedData->gpu_temp_point_3.store(tempSetpoints[2]);
            sharedData->gpu_temp_point_4.store(tempSetpoints[3]);
            sharedData->gpu_fan_point_1.store(fanSetpoints[0]);
            sharedData->gpu_fan_point_2.store(fanSetpoints[1]);
            sharedData->gpu_fan_point_3.store(fanSetpoints[2]);
            sharedData->gpu_fan_point_4.store(fanSetpoints[3]);
            sharedData->gpu_fan_point_5.store(fanSetpoints[4]);
        }
    }
    void syncFromSharedData(SharedData* sharedData, bool isCpuController)
    {
        if (isCpuController) 
        {
            hysteresis = sharedData->cpu_hysteresis.load();
            tempSetpoints[1] = sharedData->cpu_temp_point_2.load();
            tempSetpoints[2] = sharedData->cpu_temp_point_3.load();
            tempSetpoints[3] = sharedData->cpu_temp_point_4.load();
            fanSetpoints[0] = sharedData->cpu_fan_point_1.load();
            fanSetpoints[1] = sharedData->cpu_fan_point_2.load();
            fanSetpoints[2] = sharedData->cpu_fan_point_3.load();
            fanSetpoints[3] = sharedData->cpu_fan_point_4.load();
            fanSetpoints[4] = sharedData->cpu_fan_point_5.load();
        } 
        else 
        {
            hysteresis = sharedData->gpu_hysteresis.load();
            tempSetpoints[1] = sharedData->gpu_temp_point_2.load();
            tempSetpoints[2] = sharedData->gpu_temp_point_3.load();
            tempSetpoints[3] = sharedData->gpu_temp_point_4.load();
            fanSetpoints[0] = sharedData->gpu_fan_point_1.load();
            fanSetpoints[1] = sharedData->gpu_fan_point_2.load();
            fanSetpoints[2] = sharedData->gpu_fan_point_3.load();
            fanSetpoints[3] = sharedData->gpu_fan_point_4.load();
            fanSetpoints[4] = sharedData->gpu_fan_point_5.load();
			
        }
        lastTemp = 0; // Reset lastTemp to avoid stale data
    }
private:
    std::vector<int> tempSetpoints;
    std::vector<int> fanSetpoints;
    int hysteresis;
    int fanSpeed;
    double lastTemp;
};

// PID fan controller — drives fan speed to reach a target temperature.
// Gains are tuned for the PHN16-72; adjust Kp/Ki/Kd here if needed.
class PidFanController
{
public:
    static constexpr double Kp = 2.0;
    static constexpr double Ki = 0.05;
    static constexpr double Kd = 1.0;

    PidFanController() : integral(0.0), prevError(0.0), initialized(false) {}

    // Returns fan % clamped to [minFan, 100].
    // Call once per control loop tick (~1 s).
    int update(double currentTemp, double targetTemp, int minFan)
    {
        double error = currentTemp - targetTemp; // positive = too hot

        if (!initialized) {
            prevError = error;
            initialized = true;
        }

        double derivative = error - prevError; // derivative on measurement
        prevError = error;

        integral += error;

        double output = Kp * error + Ki * integral + Kd * derivative;
        int fanPct = static_cast<int>(output);

        // Clamp output
        if (fanPct < minFan)  { fanPct = minFan;  integral -= error; } // anti-windup
        if (fanPct > 100)     { fanPct = 100;      integral -= error; }

        return fanPct;
    }

    void reset()
    {
        integral    = 0.0;
        prevError   = 0.0;
        initialized = false;
    }

private:
    double integral;
    double prevError;
    bool   initialized;
};

void set_cpu_fan_manual(int percent)
{
    ec_write(cpu_fan_mode_reg, cpu_fan_mode_manual);
    ec_write(cpu_fan_speed_reg, percent);
}

void set_gpu_fan_manual(int percent)
{
    ec_write(gpu_fan_mode_reg, gpu_fan_mode_manual);
    ec_write(gpu_fan_speed_reg, percent);
}

double get_cpu_temp()
{
    return static_cast<double>(ec_read(cpu_temp_reg_1));
}

double get_gpu_temp()
{
    return static_cast<double>(ec_read(gpu_temp_reg_1));
}

std::string getRegistryValue(HKEY hKey, const std::string& subKey, const std::string& valueName) {
    HKEY hSubKey;
    std::string result = "Unknown";
    
    if (RegOpenKeyExA(hKey, subKey.c_str(), 0, KEY_READ, &hSubKey) == ERROR_SUCCESS) {
        char buffer[256];
        DWORD bufferSize = sizeof(buffer);
        DWORD type;
        
        if (RegQueryValueExA(hSubKey, valueName.c_str(), NULL, &type, (LPBYTE)buffer, &bufferSize) == ERROR_SUCCESS) {
            if (type == REG_SZ) {
                result = std::string(buffer);
            }
        }
        RegCloseKey(hSubKey);
    }
    return result;
}

bool checkSystem() 
{
    std::string manufacturer = getRegistryValue(HKEY_LOCAL_MACHINE, 
        "HARDWARE\\DESCRIPTION\\System\\BIOS", "SystemManufacturer");
    std::string model = getRegistryValue(HKEY_LOCAL_MACHINE, 
        "HARDWARE\\DESCRIPTION\\System\\BIOS", "SystemProductName");
    
    if(manufacturer == "Acer" && model == "Predator PHN16-72") 
    {
        return true;
    } 
    else 
    {
        std::string message = "Detected unsupported motherboard: " + manufacturer + " " + model;
        MessageBoxA(NULL, message.c_str(), "Unsupported System", MB_OK | MB_ICONWARNING);
        return false;
	}
}

void saveSharedDataToFile(SharedData* sharedData, const std::string& filename) 
{
    std::ofstream file(filename, std::ios::binary);
    if (!file) {
        MessageBoxA(NULL, "Could not save settings to file", "Warning", MB_OK | MB_ICONWARNING);
        return;
    }
    
    // Save non-atomic values
    double cpuTemp = sharedData->cpuTemp.load();
    double gpuTemp = sharedData->gpuTemp.load();
    int cpuFanSpeed = sharedData->cpuFanSpeed.load();
    int gpuFanSpeed = sharedData->gpuFanSpeed.load();
    int cpu_hysteresis = sharedData->cpu_hysteresis.load();
    int gpu_hysteresis = sharedData->gpu_hysteresis.load();
    int cpu_temp_point_2 = sharedData->cpu_temp_point_2.load();
    int cpu_temp_point_3 = sharedData->cpu_temp_point_3.load();
    int cpu_temp_point_4 = sharedData->cpu_temp_point_4.load();
    int gpu_temp_point_2 = sharedData->gpu_temp_point_2.load();
    int gpu_temp_point_3 = sharedData->gpu_temp_point_3.load();
    int gpu_temp_point_4 = sharedData->gpu_temp_point_4.load();
    int cpu_fan_point_1 = sharedData->cpu_fan_point_1.load();
    int cpu_fan_point_2 = sharedData->cpu_fan_point_2.load();
    int cpu_fan_point_3 = sharedData->cpu_fan_point_3.load();
    int cpu_fan_point_4 = sharedData->cpu_fan_point_4.load();
    int cpu_fan_point_5 = sharedData->cpu_fan_point_5.load();
    int gpu_fan_point_1 = sharedData->gpu_fan_point_1.load();
    int gpu_fan_point_2 = sharedData->gpu_fan_point_2.load();
    int gpu_fan_point_3 = sharedData->gpu_fan_point_3.load();
    int gpu_fan_point_4 = sharedData->gpu_fan_point_4.load();
    int gpu_fan_point_5 = sharedData->gpu_fan_point_5.load();
    int fan_ctrl_mode   = sharedData->fan_ctrl_mode.load();
    int cpu_target_temp = sharedData->cpu_target_temp.load();
    int gpu_target_temp = sharedData->gpu_target_temp.load();
    int cpu_min_fan_pid = sharedData->cpu_min_fan_pid.load();
    int gpu_min_fan_pid = sharedData->gpu_min_fan_pid.load();

    file.write(reinterpret_cast<const char*>(&cpuTemp), sizeof(cpuTemp));
    file.write(reinterpret_cast<const char*>(&gpuTemp), sizeof(gpuTemp));
    file.write(reinterpret_cast<const char*>(&cpuFanSpeed), sizeof(cpuFanSpeed));
    file.write(reinterpret_cast<const char*>(&gpuFanSpeed), sizeof(gpuFanSpeed));
    file.write(reinterpret_cast<const char*>(&cpu_hysteresis), sizeof(cpu_hysteresis));
    file.write(reinterpret_cast<const char*>(&gpu_hysteresis), sizeof(gpu_hysteresis));
    file.write(reinterpret_cast<const char*>(&cpu_temp_point_2), sizeof(cpu_temp_point_2));
    file.write(reinterpret_cast<const char*>(&cpu_temp_point_3), sizeof(cpu_temp_point_3));
    file.write(reinterpret_cast<const char*>(&cpu_temp_point_4), sizeof(cpu_temp_point_4));
    file.write(reinterpret_cast<const char*>(&gpu_temp_point_2), sizeof(gpu_temp_point_2));
    file.write(reinterpret_cast<const char*>(&gpu_temp_point_3), sizeof(gpu_temp_point_3));
    file.write(reinterpret_cast<const char*>(&gpu_temp_point_4), sizeof(gpu_temp_point_4));
    file.write(reinterpret_cast<const char*>(&cpu_fan_point_1), sizeof(cpu_fan_point_1));
    file.write(reinterpret_cast<const char*>(&cpu_fan_point_2), sizeof(cpu_fan_point_2));
    file.write(reinterpret_cast<const char*>(&cpu_fan_point_3), sizeof(cpu_fan_point_3));
    file.write(reinterpret_cast<const char*>(&cpu_fan_point_4), sizeof(cpu_fan_point_4));
    file.write(reinterpret_cast<const char*>(&cpu_fan_point_5), sizeof(cpu_fan_point_5));
    file.write(reinterpret_cast<const char*>(&gpu_fan_point_1), sizeof(gpu_fan_point_1));
    file.write(reinterpret_cast<const char*>(&gpu_fan_point_2), sizeof(gpu_fan_point_2));
    file.write(reinterpret_cast<const char*>(&gpu_fan_point_3), sizeof(gpu_fan_point_3));
    file.write(reinterpret_cast<const char*>(&gpu_fan_point_4), sizeof(gpu_fan_point_4));
    file.write(reinterpret_cast<const char*>(&gpu_fan_point_5), sizeof(gpu_fan_point_5));
    file.write(reinterpret_cast<const char*>(&fan_ctrl_mode),   sizeof(fan_ctrl_mode));
    file.write(reinterpret_cast<const char*>(&cpu_target_temp), sizeof(cpu_target_temp));
    file.write(reinterpret_cast<const char*>(&gpu_target_temp), sizeof(gpu_target_temp));
    file.write(reinterpret_cast<const char*>(&cpu_min_fan_pid), sizeof(cpu_min_fan_pid));
    file.write(reinterpret_cast<const char*>(&gpu_min_fan_pid), sizeof(gpu_min_fan_pid));
}

void loadSharedDataFromFile(SharedData* sharedData, const std::string& filename) 
{
    std::ifstream file(filename, std::ios::binary);
    if (!file) {
        // File doesn't exist, create with default values
        sharedData->cpuTemp.store(0.0);
        sharedData->gpuTemp.store(0.0);
        sharedData->cpuFanSpeed.store(0);
        sharedData->gpuFanSpeed.store(0);
        sharedData->cpu_hysteresis.store(3);
        sharedData->gpu_hysteresis.store(3);
        sharedData->cpu_temp_point_2.store(55);
        sharedData->cpu_temp_point_3.store(70);
        sharedData->cpu_temp_point_4.store(80);
        sharedData->gpu_temp_point_2.store(50);
        sharedData->gpu_temp_point_3.store(65);
        sharedData->gpu_temp_point_4.store(75);
        sharedData->cpu_fan_point_1.store(0);
        sharedData->cpu_fan_point_2.store(5);
        sharedData->cpu_fan_point_3.store(8);
        sharedData->cpu_fan_point_4.store(30);
        sharedData->cpu_fan_point_5.store(100);
        sharedData->gpu_fan_point_1.store(0);
        sharedData->gpu_fan_point_2.store(5);
        sharedData->gpu_fan_point_3.store(8);
        sharedData->gpu_fan_point_4.store(30);
        sharedData->gpu_fan_point_5.store(100);
        sharedData->write_sync.store(false);
        sharedData->fan_ctrl_mode.store(0);
        sharedData->cpu_target_temp.store(70);
        sharedData->gpu_target_temp.store(65);
        sharedData->cpu_min_fan_pid.store(10);
        sharedData->gpu_min_fan_pid.store(10);
        
        // Save default values to file
        saveSharedDataToFile(sharedData, filename);
        return;
    }
    
    // Load values from file
    double cpuTemp, gpuTemp;
    int cpuFanSpeed, gpuFanSpeed;
    int cpu_hysteresis, gpu_hysteresis;
    int cpu_temp_point_2, cpu_temp_point_3, cpu_temp_point_4;
    int gpu_temp_point_2, gpu_temp_point_3, gpu_temp_point_4;
    int cpu_fan_point_1, cpu_fan_point_2, cpu_fan_point_3, cpu_fan_point_4, cpu_fan_point_5;
    int gpu_fan_point_1, gpu_fan_point_2, gpu_fan_point_3, gpu_fan_point_4, gpu_fan_point_5;
    
    file.read(reinterpret_cast<char*>(&cpuTemp), sizeof(cpuTemp));
    file.read(reinterpret_cast<char*>(&gpuTemp), sizeof(gpuTemp));
    file.read(reinterpret_cast<char*>(&cpuFanSpeed), sizeof(cpuFanSpeed));
    file.read(reinterpret_cast<char*>(&gpuFanSpeed), sizeof(gpuFanSpeed));
    file.read(reinterpret_cast<char*>(&cpu_hysteresis), sizeof(cpu_hysteresis));
    file.read(reinterpret_cast<char*>(&gpu_hysteresis), sizeof(gpu_hysteresis));
    file.read(reinterpret_cast<char*>(&cpu_temp_point_2), sizeof(cpu_temp_point_2));
    file.read(reinterpret_cast<char*>(&cpu_temp_point_3), sizeof(cpu_temp_point_3));
    file.read(reinterpret_cast<char*>(&cpu_temp_point_4), sizeof(cpu_temp_point_4));
    file.read(reinterpret_cast<char*>(&gpu_temp_point_2), sizeof(gpu_temp_point_2));
    file.read(reinterpret_cast<char*>(&gpu_temp_point_3), sizeof(gpu_temp_point_3));
    file.read(reinterpret_cast<char*>(&gpu_temp_point_4), sizeof(gpu_temp_point_4));
    file.read(reinterpret_cast<char*>(&cpu_fan_point_1), sizeof(cpu_fan_point_1));
    file.read(reinterpret_cast<char*>(&cpu_fan_point_2), sizeof(cpu_fan_point_2));
    file.read(reinterpret_cast<char*>(&cpu_fan_point_3), sizeof(cpu_fan_point_3));
    file.read(reinterpret_cast<char*>(&cpu_fan_point_4), sizeof(cpu_fan_point_4));
    file.read(reinterpret_cast<char*>(&cpu_fan_point_5), sizeof(cpu_fan_point_5));
    file.read(reinterpret_cast<char*>(&gpu_fan_point_1), sizeof(gpu_fan_point_1));
    file.read(reinterpret_cast<char*>(&gpu_fan_point_2), sizeof(gpu_fan_point_2));
    file.read(reinterpret_cast<char*>(&gpu_fan_point_3), sizeof(gpu_fan_point_3));
    file.read(reinterpret_cast<char*>(&gpu_fan_point_4), sizeof(gpu_fan_point_4));
    file.read(reinterpret_cast<char*>(&gpu_fan_point_5), sizeof(gpu_fan_point_5));
    
    // Store loaded values
    sharedData->cpuTemp.store(cpuTemp);
    sharedData->gpuTemp.store(gpuTemp);
    sharedData->cpuFanSpeed.store(cpuFanSpeed);
    sharedData->gpuFanSpeed.store(gpuFanSpeed);
    sharedData->cpu_hysteresis.store(cpu_hysteresis);
    sharedData->gpu_hysteresis.store(gpu_hysteresis);
    sharedData->cpu_temp_point_2.store(cpu_temp_point_2);
    sharedData->cpu_temp_point_3.store(cpu_temp_point_3);
    sharedData->cpu_temp_point_4.store(cpu_temp_point_4);
    sharedData->gpu_temp_point_2.store(gpu_temp_point_2);
    sharedData->gpu_temp_point_3.store(gpu_temp_point_3);
    sharedData->gpu_temp_point_4.store(gpu_temp_point_4);
    sharedData->cpu_fan_point_1.store(cpu_fan_point_1);
    sharedData->cpu_fan_point_2.store(cpu_fan_point_2);
    sharedData->cpu_fan_point_3.store(cpu_fan_point_3);
    sharedData->cpu_fan_point_4.store(cpu_fan_point_4);
    sharedData->cpu_fan_point_5.store(cpu_fan_point_5);
    sharedData->gpu_fan_point_1.store(gpu_fan_point_1);
    sharedData->gpu_fan_point_2.store(gpu_fan_point_2);
    sharedData->gpu_fan_point_3.store(gpu_fan_point_3);
    sharedData->gpu_fan_point_4.store(gpu_fan_point_4);
    sharedData->gpu_fan_point_5.store(gpu_fan_point_5);
    sharedData->write_sync.store(false);

    // Load PID fields if present (backward compat: older files may not have them)
    int fan_ctrl_mode = 0, cpu_target_temp = 70, gpu_target_temp = 65;
    int cpu_min_fan_pid = 10, gpu_min_fan_pid = 10;
    if (file.read(reinterpret_cast<char*>(&fan_ctrl_mode),   sizeof(fan_ctrl_mode))   &&
        file.read(reinterpret_cast<char*>(&cpu_target_temp), sizeof(cpu_target_temp)) &&
        file.read(reinterpret_cast<char*>(&gpu_target_temp), sizeof(gpu_target_temp)) &&
        file.read(reinterpret_cast<char*>(&cpu_min_fan_pid), sizeof(cpu_min_fan_pid)) &&
        file.read(reinterpret_cast<char*>(&gpu_min_fan_pid), sizeof(gpu_min_fan_pid)))
    { /* loaded from file */ }
    sharedData->fan_ctrl_mode.store(fan_ctrl_mode);
    sharedData->cpu_target_temp.store(cpu_target_temp);
    sharedData->gpu_target_temp.store(gpu_target_temp);
    sharedData->cpu_min_fan_pid.store(cpu_min_fan_pid);
    sharedData->gpu_min_fan_pid.store(gpu_min_fan_pid);
}

std::string getAppDataPath() {
    char path[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, path))) {
        std::string appDataPath = std::string(path) + "\\FanControl";
        
        // Create directory if it doesn't exist
        CreateDirectoryA(appDataPath.c_str(), NULL);
        
        return appDataPath + "\\fanctrl_settings.dat";
    }
    
    // Fallback to executable directory
    char exePath[MAX_PATH];
    GetModuleFileNameA(NULL, exePath, MAX_PATH);
    std::string exeDir = std::string(exePath);
    size_t lastSlash = exeDir.find_last_of("\\");
    if (lastSlash != std::string::npos) {
        exeDir = exeDir.substr(0, lastSlash);
    }
    return exeDir + "\\fanctrl_settings.dat";
}

BOOL WINAPI ConsoleHandler(DWORD signal) {
    switch (signal) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        g_running.store(false);
        
        // Save settings before exit - use AppData path
        if (g_sharedData) {
            saveSharedDataToFile(g_sharedData, getAppDataPath());
        }
        
        // Cleanup
        if (g_sharedData) {
            UnmapViewOfFile(g_sharedData);
        }
        if (g_hMapFile) {
            CloseHandle(g_hMapFile);
        }
        
        return TRUE;
    default:
        return FALSE;
    }
}

int WINAPI WinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPSTR lpCmdLine, _In_ int nCmdShow)
{
    // Remove console allocation - comment out these lines
    // AllocConsole();
    // freopen_s((FILE**)stdout, "CONOUT$", "w", stdout);
    // freopen_s((FILE**)stderr, "CONOUT$", "w", stderr);
    // freopen_s((FILE**)stdin, "CONIN$", "r", stdin);
    
    // Set console control handler - still needed for Windows signals
    if (!SetConsoleCtrlHandler(ConsoleHandler, TRUE)) {
        // Use MessageBox instead of std::cerr
        MessageBoxA(NULL, "Could not set control handler", "Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    // Load inpoutx64.dll
    HINSTANCE hDll = LoadLibraryA("inpoutx64.dll");
    if (!hDll) {
        MessageBoxA(NULL, "Failed to load inpoutx64.dll", "Error", MB_OK | MB_ICONERROR);
        return 1;
    }
    Inp32 = (InpFunc)GetProcAddress(hDll, "Inp32");
    Out32 = (OutFunc)GetProcAddress(hDll, "Out32");
    if (!Inp32 || !Out32) {
        MessageBoxA(NULL, "Failed to get Inp32 or Out32 function", "Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    // Create shared memory for IPC
    g_hMapFile = CreateFileMapping(
        INVALID_HANDLE_VALUE,    // use paging file
        NULL,                    // default security
        PAGE_READWRITE,          // read/write access
        0,                       // max size high
        sizeof(SharedData),      // max size low
        L"MySharedMemory");      // name of mapping object

    if (g_hMapFile == NULL) {
        MessageBoxA(NULL, "Could not create file mapping object", "Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    g_sharedData = (SharedData*)MapViewOfFile(
        g_hMapFile,
        FILE_MAP_ALL_ACCESS,
        0,
        0,
        sizeof(SharedData));

    if (g_sharedData == NULL) {
        MessageBoxA(NULL, "Could not map view of file", "Error", MB_OK | MB_ICONERROR);
        CloseHandle(g_hMapFile);
        return 1;
    }

    // Initialize shared data
    g_sharedData->cpuTemp = 0.0;
    g_sharedData->gpuTemp = 0.0;
    g_sharedData->cpuFanSpeed = 0;
    g_sharedData->gpuFanSpeed = 0;

    // Initialize shared data from file - use AppData path
    std::string settingsPath = getAppDataPath();
    loadSharedDataFromFile(g_sharedData, settingsPath);

    // Print system information
    if (!checkSystem()) return -1;

    FanController cpuPID({ 0, g_sharedData->cpu_temp_point_2.load(), g_sharedData->cpu_temp_point_3.load(), g_sharedData->cpu_temp_point_4.load(), 100 }, 
                        { g_sharedData->cpu_fan_point_1.load(), g_sharedData->cpu_fan_point_2.load(), g_sharedData->cpu_fan_point_3.load(), g_sharedData->cpu_fan_point_4.load(), g_sharedData->cpu_fan_point_5.load() },
                        g_sharedData->cpu_hysteresis.load());
    FanController gpuPID({ 0, g_sharedData->gpu_temp_point_2.load(), g_sharedData->gpu_temp_point_3.load(), g_sharedData->gpu_temp_point_4.load(), 100 }, 
                        { g_sharedData->gpu_fan_point_1.load(), g_sharedData->gpu_fan_point_2.load(), g_sharedData->gpu_fan_point_3.load(), g_sharedData->gpu_fan_point_4.load(), g_sharedData->gpu_fan_point_5.load() },
                        g_sharedData->gpu_hysteresis.load());
    
    cpuPID.syncToSharedData(g_sharedData, true);
    gpuPID.syncToSharedData(g_sharedData, false);

    PidFanController cpuPid;
    PidFanController gpuPid;

    LowPassFilter cpuFilter(0.1);
    LowPassFilter gpuFilter(0.1);

    int cpuFanLast = -1, gpuFanLast = -1;

    // Remove this console message
    // std::cout << "Fan control started. Press Ctrl+C to stop." << std::endl;

    while (g_running.load()) 
    {
        #define retry_count 3

        double cpuTemp = cpuFilter.filter(get_cpu_temp());
        for (int i = 0; (cpuTemp == 0 || cpuTemp > 110) && i < retry_count + 1; i++)
        {
            if (i != retry_count) cpuTemp = cpuFilter.filter(get_cpu_temp());
            else cpuTemp = 100;
        }

        double gpuTemp = gpuFilter.filter(get_gpu_temp());
        for (int i = 0; (gpuTemp == 0 || gpuTemp > 110) && i < retry_count + 1; i++)
        {
            if (i != retry_count) gpuTemp = gpuFilter.filter(get_gpu_temp());
            else gpuTemp = 100;
        }

        int cpuFan, gpuFan;
        if (g_sharedData->fan_ctrl_mode.load() == 1)
        {
            // PID mode: drive temps toward target
            int cpuTarget  = g_sharedData->cpu_target_temp.load();
            int gpuTarget  = g_sharedData->gpu_target_temp.load();
            int cpuMinPid  = g_sharedData->cpu_min_fan_pid.load();
            int gpuMinPid  = g_sharedData->gpu_min_fan_pid.load();
            cpuFan = cpuPid.update(cpuTemp, cpuTarget, cpuMinPid);
            gpuFan = gpuPid.update(gpuTemp, gpuTarget, gpuMinPid);
        }
        else
        {
            // Curve mode
            cpuFan = cpuPID.update(cpuTemp);
            gpuFan = gpuPID.update(gpuTemp);
        }

        if (cpuFan > gpuFan + 10) gpuFan = cpuFan - 10;
        else if (gpuFan > cpuFan + 10) cpuFan = gpuFan - 10;

        if (cpuFan != cpuFanLast) {
            set_cpu_fan_manual(cpuFan);
            cpuFanLast = cpuFan;
        }
        if (gpuFan != gpuFanLast) {
            set_gpu_fan_manual(gpuFan);
            gpuFanLast = gpuFan;
        }

        g_sharedData->cpuTemp.store(cpuTemp);
        g_sharedData->gpuTemp.store(gpuTemp);
        g_sharedData->cpuFanSpeed.store(cpuFan);
        g_sharedData->gpuFanSpeed.store(gpuFan);

        // Use shorter sleep and check for exit condition
        for (int i = 0; i < 10 && g_running.load(); i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        if (g_sharedData->write_sync.load())
        {
            cpuPID.syncFromSharedData(g_sharedData, true);
            gpuPID.syncFromSharedData(g_sharedData, false);
            cpuPid.reset();
            gpuPid.reset();
            g_sharedData->write_sync.store(false);
            
            // Save settings when they change - use AppData path
            saveSharedDataToFile(g_sharedData, settingsPath);
        }
    }
    
    // Remove these console messages
    // std::cout << "Cleaning up..." << std::endl;
    
    // Save before exit - use AppData path
    saveSharedDataToFile(g_sharedData, settingsPath);
    
    UnmapViewOfFile(g_sharedData);
    CloseHandle(g_hMapFile);
    
    // Remove console cleanup
    // FreeConsole();
    return 0;
}
