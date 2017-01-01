#include "Compute.h"
#include "ImageLogger.h"
#include "VideoSource.h"
#include <iostream>
#include <opencv2/imgproc.hpp>


enum SegMethod {Threshold, Edge};
static const SegMethod seg = Threshold;

static const bool thresholdCalibrating = false;


template<typename T>
static inline T square(T x) {
    return x * x;
}

template<typename T>
static inline T length2(const cv::Vec<T,4>& v) {
    return square(v(0) - v(2)) + square(v(1) - v(3));
}

static bool longer(const cv::Vec4i& v, const cv::Vec4i& w) {
    return length2(v) > length2(w);
}

void OutputData::UseLineForDirection(const cv::Vec4i& line) {
    if (line(1) < line(3)) {
        loX = line(0);
        loY = line(1);
        hiX = line(2);
        hiY = line(3);
    } else {
        hiX = line(0);
        hiY = line(1);
        loX = line(2);
        loY = line(3);
    }

    if (hiY > image.rows / 20) {
        const int mid = image.cols / 2;
        direction = mid < loX && loX < hiX || mid > loX && loX > hiX
                  ? Turn
                  : GoStraight;
    }
}

typedef std::vector<cv::Point> Polygon;
typedef std::vector<Polygon> ContoursT;

void Compute::BackgroundLoop() {
    const double rho = 2., theta = 0.02;
    const int minLineLength = cap->imageHeight / 3;

    const double maxThresholdArea = cap->imageWidth * cap->imageHeight / 12;

    const int HoughThreshold = cap->imageWidth * cap->imageHeight / 6500;
    const double HoughMaxGap = 25;

    cv::Mat bw(cap->imageHeight/2, cap->imageWidth, CV_8UC1);
    cv::Mat lab[3];
    static const int toGray[3*2] = {0,0, 0,1, 0,2};
    std::vector<cv::Vec4f> newLines;
    cv::Vec4f line;
    double totalThreshold = 0., threshold = 0.;
    int goodThresholdFrames = 0;

    for (int i = 0; ! cap->imagesCaptured.quitting; ++i) {
        cv::Mat inp(cap->imagesCaptured.Dequeue());
        if (! inp.empty()) {

            if (log && i % 5 == 0 && log->imagesToLog.size < log->imagesToLog.Capacity()) {
                log->imagesToLog.Enqueue(inp.clone());
            }

            OutputData out;
            out.image = inp;

            inp = inp.rowRange(0, cap->imageHeight/2);
            cv::cvtColor(inp, inp, cv::COLOR_BGR2Lab);

            if (seg == Threshold) {
                static const int pickB[1*2] = {2,0};
                cv::mixChannels(&inp, 1, &bw, 1, pickB, 1);
                threshold = cv::threshold(bw, bw, 128, 255, cv::THRESH_BINARY_INV|cv::THRESH_OTSU);

                ContoursT contours;
                cv::findContours(bw, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
                double area = 0.;
                for (ContoursT::const_iterator p = contours.begin();
                     area < maxThresholdArea && p != contours.end();
                     ++p) {
                    area += cv::contourArea(*p);
                }
                if (area < maxThresholdArea) {
                    for (ContoursT::iterator p = contours.begin(); p != contours.end(); ++p) {
                        Polygon& poly = *p;
                        cv::fitLine(poly, line, cv::DIST_L2, 0., rho, theta);
                        cv::Point p1(-9999*line(0) + line(2), -9999*line(1) + line(3)),
                                  p2( 9999*line(0) + line(2),  9999*line(1) + line(3));
                        cv::clipLine(cv::boundingRect(poly), p1, p2);
                        out.lines.push_back(cv::Vec4i(p1.x, p1.y, p2.x, p2.y));
                    }
                }

                cv::mixChannels(&inp, 1, &inp, 1, toGray, 3);
                cv::drawContours(out.image, contours, -1, cv::Scalar(0,255,0));
            }
            else {
                cv::split(inp, lab);
                for (int ab = 1; ab <= 2; ++ab) {
                    cv::Canny(lab[ab], bw, 180., 200., 5);
                    cv::HoughLinesP(bw, newLines, rho, theta, HoughThreshold, minLineLength, HoughMaxGap);
                    out.lines.insert(out.lines.end(), newLines.begin(), newLines.end());
                }

                cv::mixChannels(lab+0, 1, &inp, 1, toGray, 3);
            }

            std::sort(out.lines.begin(), out.lines.end(), longer);
            out.direction = GoBack;
            for (Lines::iterator l = out.lines.begin();
                 out.direction != Turn && l != out.lines.end() && length2(*l) > square(minLineLength);
                 ++l) {
                out.UseLineForDirection(*l);
            }
            if (out.direction != GoBack && seg == Threshold) {
                totalThreshold += threshold;
                ++goodThresholdFrames;
                if (thresholdCalibrating) {
                    std::cerr << "threshold: " << threshold << std::endl;
                }
            }

            SwapOutputData(out);
        }
    }

    if (goodThresholdFrames > 0) {
        std::cerr << "average threshold: " << totalThreshold / goodThresholdFrames << std::endl;
    }
}

static void* ComputeThread(void* compute) {
    ((Compute*)compute)->BackgroundLoop();
    return 0;
}

void Compute::Start() {
    pthread_create(&thread, NULL, ComputeThread, this);
}

void Compute::Stop() {
    cap->imagesCaptured.Quit();
    pthread_join(thread, NULL);
}

Compute::Compute(VideoSource* c, ImageLogger* lg): cap(c), log(lg) {
    pthread_mutex_init(&dataMutex, NULL);
}

Compute::~Compute() {
    pthread_mutex_destroy(&dataMutex);
}

void Compute::SwapOutputData(OutputData& data) {
    OutputData x = data;

    pthread_mutex_lock(&dataMutex);
    data = outputData;
    outputData = x;
    pthread_mutex_unlock(&dataMutex);
}
