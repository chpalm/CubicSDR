// Copyright (c) Charles J. Cliffe
// SPDX-License-Identifier: GPL-2.0+

#include "SpectrumVisualProcessor.h"
#include "CubicSDR.h"


SpectrumVisualProcessor::SpectrumVisualProcessor() : outputBuffers("SpectrumVisualProcessorBuffers") {
    lastInputBandwidth = 0;
    lastBandwidth = 0;
    lastDataSize = 0;
    resampler = nullptr;
    resamplerRatio = 0;

    fftInput = nullptr;
    fftOutput = nullptr;
    fftInData = nullptr;
    fftLastData = nullptr;
    fftPlan = nullptr;
    
    is_view.store(false);
    fftSize.store(0);
    centerFreq.store(0);
    bandwidth.store(0);
    hideDC.store(false);
    
    freqShifter = nco_crcf_create(LIQUID_NCO);
    shiftFrequency = 0;
    
    fft_ceil_ma = fft_ceil_maa = 100.0;
    fft_floor_ma = fft_floor_maa = 0.0;
    fft_floor_peak = 0.0;
    desiredInputSize.store(0);
    fft_average_rate = 0.65f;
    scaleFactor.store(1.0);
    fftSizeChanged.store(false);
    newFFTSize.store(0);
    lastView = false;
    peakHold.store(false);
    peakReset.store(false);
    
}

SpectrumVisualProcessor::~SpectrumVisualProcessor() {
    nco_crcf_destroy(freqShifter);
}

bool SpectrumVisualProcessor::isView() {
    return is_view.load();
}

void SpectrumVisualProcessor::setView(bool bView) {
    
    std::lock_guard < std::mutex > busy_lock(busy_run);    
    is_view.store(bView);
    
}

void SpectrumVisualProcessor::setView(bool bView, long long centerFreq_in, long bandwidth_in) {
    
    std::lock_guard < std::mutex > busy_lock(busy_run);    
    is_view.store(bView);
    bandwidth.store(bandwidth_in);
    centerFreq.store(centerFreq_in);
   
}


void SpectrumVisualProcessor::setFFTAverageRate(float fftAverageRate) {
   
    std::lock_guard < std::mutex > busy_lock(busy_run);    
    this->fft_average_rate.store(fftAverageRate);
    
}

float SpectrumVisualProcessor::getFFTAverageRate() {
    return this->fft_average_rate.load();
}

void SpectrumVisualProcessor::setCenterFrequency(long long centerFreq_in) {
   
    std::lock_guard < std::mutex > busy_lock(busy_run);    
    centerFreq.store(centerFreq_in);
   
}

long long SpectrumVisualProcessor::getCenterFrequency() {
    return centerFreq.load();
}

void SpectrumVisualProcessor::setBandwidth(long bandwidth_in) {
   
    std::lock_guard < std::mutex > busy_lock(busy_run);    
    bandwidth.store(bandwidth_in);
   
}

long SpectrumVisualProcessor::getBandwidth() {
    return bandwidth.load();
}

void SpectrumVisualProcessor::setPeakHold(bool peakHold_in) {
   
    if (peakHold.load() && peakHold_in) {
        peakReset.store(PEAK_RESET_COUNT);
    } else {
        peakHold.store(peakHold_in);
        peakReset.store(1);
    }
}

bool SpectrumVisualProcessor::getPeakHold() {
    return peakHold.load();
}

int SpectrumVisualProcessor::getDesiredInputSize() {
    return desiredInputSize.load();
}

void SpectrumVisualProcessor::setup(unsigned int fftSize_in) {

    std::lock_guard < std::mutex > busy_lock(busy_run);    

    fftSize = fftSize_in;
    fftSizeInternal = fftSize_in * SPECTRUM_VZM;
    lastDataSize = 0;

    int memSize = sizeof(liquid_float_complex) * fftSizeInternal;
    
    if (fftInput) {
        free(fftInput);
    }
    fftInput = (liquid_float_complex*)malloc(memSize);
    memset(fftInput,0,memSize);
    
    if (fftInData) {
        free(fftInData);
    }
    fftInData = (liquid_float_complex*)malloc(memSize);
    memset(fftInput,0,memSize);
    
    if (fftLastData) {
        free(fftLastData);
    }
    fftLastData = (liquid_float_complex*)malloc(memSize);
    memset(fftInput,0,memSize);
    
    if (fftOutput) {
        free(fftOutput);
    }
    fftOutput = (liquid_float_complex*)malloc(memSize);
    memset(fftInput,0,memSize);
    
    if (fftPlan) {
        fft_destroy_plan(fftPlan);
    }
    fftPlan = fft_create_plan(fftSizeInternal, fftInput, fftOutput, LIQUID_FFT_FORWARD, 0);
    
}

void SpectrumVisualProcessor::setFFTSize(unsigned int fftSize_in) {
    if (fftSize_in == fftSize) {
        return;
    }
    newFFTSize = fftSize_in;
    fftSizeChanged.store(true);
}

unsigned int SpectrumVisualProcessor::getFFTSize() {
    if (fftSizeChanged.load()) {
        return newFFTSize;
    }
    return fftSize.load();
}


void SpectrumVisualProcessor::setHideDC(bool hideDC) {
    this->hideDC.store(hideDC);
}


void SpectrumVisualProcessor::process() {
    if (!isOutputEmpty()) {
        return;
    }
    if (!input || input->empty()) {
        return;
    }
    
    if (fftSizeChanged.load()) {
        setup(newFFTSize);
        fftSizeChanged.store(false);
    }

    DemodulatorThreadIQData *iqData;
    
    input->pop(iqData);
    
    if (!iqData) {
        return;
    }

   
    //Start by locking concurrent access to iqData
    std::lock_guard < std::recursive_mutex > lock(iqData->getMonitor());

    //then get the busy_lock
    std::lock_guard < std::mutex > busy_lock(busy_run);    


   
    bool doPeak = peakHold.load() && (peakReset.load() == 0);
    
    if (fft_result.size() != fftSizeInternal) {
        if (fft_result.capacity() < fftSizeInternal) {
            fft_result.reserve(fftSizeInternal);
            fft_result_ma.reserve(fftSizeInternal);
            fft_result_maa.reserve(fftSizeInternal);
            fft_result_peak.reserve(fftSizeInternal);
        }
        fft_result.resize(fftSizeInternal);
        fft_result_ma.resize(fftSizeInternal);
        fft_result_maa.resize(fftSizeInternal);
        fft_result_temp.resize(fftSizeInternal);
        fft_result_peak.resize(fftSizeInternal);
    }
    
    if (peakReset.load() != 0) {
        peakReset--;
        if (peakReset.load() == 0) {
            for (unsigned int i = 0, iMax = fftSizeInternal; i < iMax; i++) {
                fft_result_peak[i] = fft_floor_maa;
            }
            fft_ceil_peak = fft_floor_maa;
            fft_floor_peak = fft_ceil_maa;
        }
    }
    
    std::vector<liquid_float_complex> *data = &iqData->data;
    
    if (data && data->size()) {
        unsigned int num_written;
        long resampleBw = iqData->sampleRate;
        bool newResampler = false;
        int bwDiff;
        
        if (is_view.load()) {
            if (!iqData->sampleRate) {
                iqData->decRefCount();
               
                return;
            }
            
            while (resampleBw / SPECTRUM_VZM >= bandwidth) {
                resampleBw /= SPECTRUM_VZM;
            }
            
            resamplerRatio = (double) (resampleBw) / (double) iqData->sampleRate;
            
            size_t desired_input_size = fftSizeInternal / resamplerRatio;
            
            this->desiredInputSize.store(desired_input_size);
            
            if (iqData->data.size() < desired_input_size) {
                //                std::cout << "fft underflow, desired: " << desired_input_size << " actual:" << input->data.size() << std::endl;
                desired_input_size = iqData->data.size();
            }
            
            if (centerFreq != iqData->frequency) {
                if ((centerFreq - iqData->frequency) != shiftFrequency || lastInputBandwidth != iqData->sampleRate) {
                    if (abs(iqData->frequency - centerFreq) < (wxGetApp().getSampleRate() / 2)) {
                        long lastShiftFrequency = shiftFrequency;
                        shiftFrequency = centerFreq - iqData->frequency;
                        nco_crcf_set_frequency(freqShifter, (2.0 * M_PI) * (((double) abs(shiftFrequency)) / ((double) iqData->sampleRate)));
                        
                        if (is_view.load()) {
                            long freqDiff = shiftFrequency - lastShiftFrequency;
                            
                            if (lastBandwidth!=0) {
                                double binPerHz = double(lastBandwidth) / double(fftSizeInternal);
                                
                                unsigned int numShift = floor(double(abs(freqDiff)) / binPerHz);
                                
                                if (numShift < fftSizeInternal/2 && numShift) {
                                    if (freqDiff > 0) {
                                        memmove(&fft_result_ma[0], &fft_result_ma[numShift], (fftSizeInternal-numShift) * sizeof(double));
                                        memmove(&fft_result_maa[0], &fft_result_maa[numShift], (fftSizeInternal-numShift) * sizeof(double));
//                                        memmove(&fft_result_peak[0], &fft_result_peak[numShift], (fftSizeInternal-numShift) * sizeof(double));
//                                        memset(&fft_result_peak[fftSizeInternal-numShift], 0, numShift * sizeof(double));
                                    } else {
                                        memmove(&fft_result_ma[numShift], &fft_result_ma[0], (fftSizeInternal-numShift) * sizeof(double));
                                        memmove(&fft_result_maa[numShift], &fft_result_maa[0], (fftSizeInternal-numShift) * sizeof(double));
//                                        memmove(&fft_result_peak[numShift], &fft_result_peak[0], (fftSizeInternal-numShift) * sizeof(double));
//                                        memset(&fft_result_peak[0], 0, numShift * sizeof(double));
                                    }
                                }
                            }
                        }
                    }
                    peakReset.store(PEAK_RESET_COUNT);
                }
                
                if (shiftBuffer.size() != desired_input_size) {
                    if (shiftBuffer.capacity() < desired_input_size) {
                        shiftBuffer.reserve(desired_input_size);
                    }
                    shiftBuffer.resize(desired_input_size);
                }
                
                if (shiftFrequency < 0) {
                    nco_crcf_mix_block_up(freqShifter, &iqData->data[0], &shiftBuffer[0], desired_input_size);
                } else {
                    nco_crcf_mix_block_down(freqShifter, &iqData->data[0], &shiftBuffer[0], desired_input_size);
                }
            } else {
                shiftBuffer.assign(iqData->data.begin(), iqData->data.begin()+desired_input_size);
            }
            
            if (!resampler || resampleBw != lastBandwidth || lastInputBandwidth != iqData->sampleRate) {
                float As = 480.0;
                
                if (resampler) {
                    msresamp_crcf_destroy(resampler);
                }
                
                resampler = msresamp_crcf_create(resamplerRatio, As);
                
                bwDiff = resampleBw-lastBandwidth;
                lastBandwidth = resampleBw;
                lastInputBandwidth = iqData->sampleRate;
                newResampler = true;
                peakReset.store(PEAK_RESET_COUNT);
            }
            
            
            unsigned int out_size = ceil((double) (desired_input_size) * resamplerRatio) + 512;
            
            if (resampleBuffer.size() != out_size) {
                if (resampleBuffer.capacity() < out_size) {
                    resampleBuffer.reserve(out_size);
                }
                resampleBuffer.resize(out_size);
            }
            
            msresamp_crcf_execute(resampler, &shiftBuffer[0], desired_input_size, &resampleBuffer[0], &num_written);
            
            if (num_written < fftSizeInternal) {
                memcpy(fftInData, resampleBuffer.data(), num_written * sizeof(liquid_float_complex));
                memset(&(fftInData[num_written]), 0, (fftSizeInternal-num_written) * sizeof(liquid_float_complex));
            } else {
                memcpy(fftInData, resampleBuffer.data(), fftSizeInternal * sizeof(liquid_float_complex));
            }
        } else {
            this->desiredInputSize.store(fftSizeInternal);

            num_written = data->size();
            if (data->size() < fftSizeInternal) {
                memcpy(fftInData, data->data(), data->size() * sizeof(liquid_float_complex));
                memset(&fftInData[data->size()], 0, (fftSizeInternal - data->size()) * sizeof(liquid_float_complex));
            } else {
                memcpy(fftInData, data->data(), fftSizeInternal * sizeof(liquid_float_complex));
            }
        }
        
        bool execute = false;

        if (num_written >= fftSizeInternal) {
            execute = true;
            memcpy(fftInput, fftInData, fftSizeInternal * sizeof(liquid_float_complex));
            memcpy(fftLastData, fftInput, fftSizeInternal * sizeof(liquid_float_complex));
            
        } else {
            if (lastDataSize + num_written < fftSizeInternal) { // priming
                unsigned int num_copy = fftSizeInternal - lastDataSize;
                if (num_written > num_copy) {
                    num_copy = num_written;
                }
                memcpy(fftLastData, fftInData, num_copy * sizeof(liquid_float_complex));
                lastDataSize += num_copy;
            } else {
                unsigned int num_last = (fftSizeInternal - num_written);
                memcpy(fftInput, fftLastData + (lastDataSize - num_last), num_last * sizeof(liquid_float_complex));
                memcpy(fftInput + num_last, fftInData, num_written * sizeof(liquid_float_complex));
                memcpy(fftLastData, fftInput, fftSizeInternal * sizeof(liquid_float_complex));
                execute = true;
            }
        }
        
        if (execute) {
            SpectrumVisualData *output = outputBuffers.getBuffer();
            
            if (output->spectrum_points.size() != fftSize * 2) {
                output->spectrum_points.resize(fftSize * 2);
            }
            if (doPeak) {
                if (output->spectrum_hold_points.size() != fftSize * 2) {
                    output->spectrum_hold_points.resize(fftSize * 2);
                }
            } else {
                output->spectrum_hold_points.resize(0);
            }
            
            float fft_ceil = 0, fft_floor = 1;

            fft_execute(fftPlan);
            
            for (int i = 0, iMax = fftSizeInternal / 2; i < iMax; i++) {
                float a = fftOutput[i].real;
                float b = fftOutput[i].imag;
                float c = sqrt(a * a + b * b);
                
                float x = fftOutput[fftSizeInternal / 2 + i].real;
                float y = fftOutput[fftSizeInternal / 2 + i].imag;
                float z = sqrt(x * x + y * y);
                
                fft_result[i] = (z);
                fft_result[fftSizeInternal / 2 + i] = (c);
            }
            
            if (newResampler && lastView) {
                if (bwDiff < 0) {
                    for (unsigned int i = 0, iMax = fftSizeInternal; i < iMax; i++) {
                        fft_result_temp[i] = fft_result_ma[(fftSizeInternal/4) + (i/2)];
                    }
                    for (unsigned int i = 0, iMax = fftSizeInternal; i < iMax; i++) {
                        fft_result_ma[i] = fft_result_temp[i];
                        
                        fft_result_temp[i] = fft_result_maa[(fftSizeInternal/4) + (i/2)];
                    }
                    for (unsigned int i = 0, iMax = fftSizeInternal; i < iMax; i++) {
                        fft_result_maa[i] = fft_result_temp[i];
                    }
                } else {
                    for (size_t i = 0, iMax = fftSizeInternal; i < iMax; i++) {
                        if (i < fftSizeInternal/4) {
                            fft_result_temp[i] = 0; // fft_result_ma[fftSizeInternal/4];
                        } else if (i >= fftSizeInternal - fftSizeInternal/4) {
                            fft_result_temp[i] = 0; // fft_result_ma[fftSizeInternal - fftSizeInternal/4-1];
                        } else {
                            fft_result_temp[i] = fft_result_ma[(i-fftSizeInternal/4)*2];
                        }
                    }
                    for (unsigned int i = 0, iMax = fftSizeInternal; i < iMax; i++) {
                        fft_result_ma[i] = fft_result_temp[i];
                        
                        if (i < fftSizeInternal/4) {
                            fft_result_temp[i] = 0; //fft_result_maa[fftSizeInternal/4];
                        } else if (i >= fftSizeInternal - fftSizeInternal/4) {
                            fft_result_temp[i] = 0; // fft_result_maa[fftSizeInternal - fftSizeInternal/4-1];
                        } else {
                            fft_result_temp[i] = fft_result_maa[(i-fftSizeInternal/4)*2];
                        }
                    }
                    for (unsigned int i = 0, iMax = fftSizeInternal; i < iMax; i++) {
                        fft_result_maa[i] = fft_result_temp[i];
                    }
                }
            }
            
            for (int i = 0, iMax = fftSizeInternal; i < iMax; i++) {
                if (fft_result_maa[i] != fft_result_maa[i]) fft_result_maa[i] = fft_result[i];
                fft_result_maa[i] += (fft_result_ma[i] - fft_result_maa[i]) * fft_average_rate;
                if (fft_result_ma[i] != fft_result_ma[i]) fft_result_ma[i] = fft_result[i];
                fft_result_ma[i] += (fft_result[i] - fft_result_ma[i]) * fft_average_rate;
                
                if (fft_result_maa[i] > fft_ceil || fft_ceil != fft_ceil) {
                    fft_ceil = fft_result_maa[i];
                }
                if (fft_result_maa[i] < fft_floor || fft_floor != fft_floor) {
                    fft_floor = fft_result_maa[i];
                }
                if (doPeak) {
                    if (fft_result_maa[i] > fft_result_peak[i]) {
                        fft_result_peak[i] = fft_result_maa[i];
                    }
                }
            }
            
            if (fft_ceil_ma != fft_ceil_ma) fft_ceil_ma = fft_ceil;
            fft_ceil_ma = fft_ceil_ma + (fft_ceil - fft_ceil_ma) * 0.05;
            if (fft_ceil_maa != fft_ceil_maa) fft_ceil_maa = fft_ceil;
            fft_ceil_maa = fft_ceil_maa + (fft_ceil_ma - fft_ceil_maa) * 0.05;
            
            if (fft_floor_ma != fft_floor_ma) fft_floor_ma = fft_floor;
            fft_floor_ma = fft_floor_ma + (fft_floor - fft_floor_ma) * 0.05;
            if (fft_floor_maa != fft_floor_maa) fft_floor_maa = fft_floor;
            fft_floor_maa = fft_floor_maa + (fft_floor_ma - fft_floor_maa) * 0.05;

            if (doPeak) {
                if (fft_ceil_maa > fft_ceil_peak) {
                    fft_ceil_peak = fft_ceil_maa;
                }
                if (fft_floor_maa < fft_floor_peak) {
                    fft_floor_peak = fft_floor_maa;
                }
            }
            
            float sf = scaleFactor.load();
 
            double visualRatio = (double(bandwidth) / double(resampleBw));
            double visualStart = (double(fftSizeInternal) / 2.0) - (double(fftSizeInternal) * (visualRatio / 2.0));
            double visualAccum = 0;
            double peak_acc = 0, acc = 0, accCount = 0, i = 0;
   
            double point_ceil = doPeak?fft_ceil_peak:fft_ceil_maa;
            double point_floor = doPeak?fft_floor_peak:fft_floor_maa;
            
            for (int x = 0, xMax = output->spectrum_points.size() / 2; x < xMax; x++) {
                visualAccum += visualRatio * double(SPECTRUM_VZM);

                while (visualAccum >= 1.0) {
                    unsigned int idx = round(visualStart+i);
                    if (idx > 0 && idx < fftSizeInternal) {
                        acc += fft_result_maa[idx];
                        if (doPeak) {
                            peak_acc += fft_result_peak[idx];
                        }
                    } else {
                        acc += fft_floor_maa;
                        if (doPeak) {
                            peak_acc += fft_floor_maa;
                        }
                    }
                    accCount += 1.0;
                    visualAccum -= 1.0;
                    i++;
                }

                output->spectrum_points[x * 2] = ((float) x / (float) xMax);
                if (doPeak) {
                    output->spectrum_hold_points[x * 2] = ((float) x / (float) xMax);
                }
                if (accCount) {
                    output->spectrum_points[x * 2 + 1] = ((log10((acc/accCount)+0.25 - (point_floor-0.75)) / log10((point_ceil+0.25) - (point_floor-0.75))))*sf;
                    acc = 0.0;
                    if (doPeak) {
                        output->spectrum_hold_points[x * 2 + 1] = ((log10((peak_acc/accCount)+0.25 - (point_floor-0.75)) / log10((point_ceil+0.25) - (point_floor-0.75))))*sf;
                        peak_acc = 0.0;
                    }
                    accCount = 0.0;
                }
            }
            
            if (hideDC.load()) { // DC-spike removal
                long long freqMin = centerFreq-(bandwidth/2);
                long long freqMax = centerFreq+(bandwidth/2);
                long long zeroPt = (iqData->frequency-freqMin);
                
                if (freqMin < iqData->frequency && freqMax > iqData->frequency) {
                    int freqRange = int(freqMax-freqMin);
                    int freqStep = freqRange/fftSize;
                    int fftStart = (zeroPt/freqStep)-(2000/freqStep);
                    int fftEnd = (zeroPt/freqStep)+(2000/freqStep);
                    
//                    std::cout << "range:" << freqRange << ", step: " << freqStep << ", start: " << fftStart << ", end: " << fftEnd << std::endl;
                    
                    if (fftEnd-fftStart < 2) {
                        fftEnd++;
                        fftStart--;
                    }
                    
                    int numSteps = (fftEnd-fftStart);
                    int halfWay = fftStart+(numSteps/2);

                    if ((fftEnd+numSteps/2+1 < (long long) fftSize) && (fftStart-numSteps/2-1 >= 0) && (fftEnd > fftStart)) {
                        int n = 1;
                        for (int i = fftStart; i < halfWay; i++) {
                            output->spectrum_points[i * 2 + 1] = output->spectrum_points[(fftStart - n) * 2 + 1];
                            n++;
                        }
                        n = 1;
                        for (int i = halfWay; i < fftEnd; i++) {
                            output->spectrum_points[i * 2 + 1] = output->spectrum_points[(fftEnd + n) * 2 + 1];
                            n++;
                        }
                        if (doPeak) {
                            int n = 1;
                            for (int i = fftStart; i < halfWay; i++) {
                                output->spectrum_hold_points[i * 2 + 1] = output->spectrum_hold_points[(fftStart - n) * 2 + 1];
                                n++;
                            }
                            n = 1;
                            for (int i = halfWay; i < fftEnd; i++) {
                                output->spectrum_hold_points[i * 2 + 1] = output->spectrum_hold_points[(fftEnd + n) * 2 + 1];
                                n++;
                            }
                        }
                    }
                }
            }
            
            output->fft_ceiling = point_ceil/sf;
            output->fft_floor = point_floor;

            output->centerFreq = centerFreq;
            output->bandwidth = bandwidth;

            distribute(output);
        }
    }
 
    iqData->decRefCount();
   
    
    lastView = is_view.load();
}


void SpectrumVisualProcessor::setScaleFactor(float sf) {
    scaleFactor.store(sf);
}


float SpectrumVisualProcessor::getScaleFactor() {
    return scaleFactor.load();
}

