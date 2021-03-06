// Copyright (C) 2019  Joseph Artsimovich <joseph.artsimovich@gmail.com>, 4lex4 <4lex49@zoho.com>
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.

#include "UnitsProvider.h"

#include <algorithm>

#include "ApplicationSettings.h"
#include "Dpm.h"
#include "UnitsConverter.h"

UnitsProvider::UnitsProvider() : m_units(unitsFromString(ApplicationSettings::getInstance().getUnits())) {}

UnitsProvider& UnitsProvider::getInstance() {
  static UnitsProvider instance;
  return instance;
}

void UnitsProvider::setUnits(Units units) {
  UnitsProvider::m_units = units;
  unitsChanged();
}

void UnitsProvider::addListener(UnitsListener* listener) {
  m_unitsListeners.push_back(listener);
}

void UnitsProvider::removeListener(UnitsListener* listener) {
  m_unitsListeners.remove(listener);
}

void UnitsProvider::unitsChanged() {
  for (UnitsListener* listener : m_unitsListeners) {
    listener->onUnitsChanged(m_units);
  }
}

void UnitsProvider::convertFrom(double& horizontalValue, double& verticalValue, Units fromUnits, const Dpi& dpi) const {
  UnitsConverter(dpi).convert(horizontalValue, verticalValue, fromUnits, m_units);
}

void UnitsProvider::convertTo(double& horizontalValue, double& verticalValue, Units toUnits, const Dpi& dpi) const {
  UnitsConverter(dpi).convert(horizontalValue, verticalValue, m_units, toUnits);
}
