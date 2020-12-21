#pragma once

#include "vinylcontrol/vinylcontrol.h"
#include "ywax.h"

class VinylControlYwax : public VinylControl {
  public:
    VinylControlYwax(UserSettingsPointer pConfig, const QString& group);
    virtual ~VinylControlYwax();

    void analyzeSamples(CSAMPLE* pSamples, size_t nFrames);

    virtual bool writeQualityReport(VinylSignalQualityReport* qualityReportFifo);

  protected:
    float getAngle();

  private:
    Ywax* ywax;
    bool checkEnabled(bool was, bool is);

    // The Vinyl Control mode and the previous mode.
    int m_iVCMode;
    int m_iOldVCMode;
};
