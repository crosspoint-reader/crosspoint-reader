#pragma once

#include <cstdint>

// Structure of the Rosary:
// 1. Sign of the Cross + Apostles' Creed
// 2. Our Father
// 3. 3x Hail Mary (for faith, hope, charity)
// 4. Glory Be
// 5. For each of 5 decades:
//    a. Announce Mystery
//    b. Our Father
//    c. 10x Hail Mary
//    d. Glory Be
//    e. Fatima Prayer (O My Jesus)
// 6. Hail Holy Queen
// 7. Final Prayer
// 8. Sign of the Cross

namespace RosaryData {

// Days of the week
enum class DayOfWeek : uint8_t { Sunday, Monday, Tuesday, Wednesday, Thursday, Friday, Saturday };

// Mystery sets
enum class MysterySet : uint8_t { Joyful, Sorrowful, Glorious, Luminous };

inline const char* getMysterySetName(MysterySet set) {
  switch (set) {
    case MysterySet::Joyful:
      return "Joyful Mysteries";
    case MysterySet::Sorrowful:
      return "Sorrowful Mysteries";
    case MysterySet::Glorious:
      return "Glorious Mysteries";
    case MysterySet::Luminous:
      return "Luminous Mysteries";
    default:
      return "";
  }
}

inline MysterySet getMysterySetForDay(DayOfWeek day) {
  switch (day) {
    case DayOfWeek::Monday:
    case DayOfWeek::Saturday:
      return MysterySet::Joyful;
    case DayOfWeek::Tuesday:
    case DayOfWeek::Friday:
      return MysterySet::Sorrowful;
    case DayOfWeek::Wednesday:
    case DayOfWeek::Sunday:
      return MysterySet::Glorious;
    case DayOfWeek::Thursday:
      return MysterySet::Luminous;
    default:
      return MysterySet::Joyful;
  }
}

inline const char* getDayName(DayOfWeek day) {
  switch (day) {
    case DayOfWeek::Sunday:
      return "Sunday";
    case DayOfWeek::Monday:
      return "Monday";
    case DayOfWeek::Tuesday:
      return "Tuesday";
    case DayOfWeek::Wednesday:
      return "Wednesday";
    case DayOfWeek::Thursday:
      return "Thursday";
    case DayOfWeek::Friday:
      return "Friday";
    case DayOfWeek::Saturday:
      return "Saturday";
    default:
      return "";
  }
}

// Mystery names for each set (5 mysteries per set)
inline const char* getMysteryName(MysterySet set, int index) {
  switch (set) {
    case MysterySet::Joyful:
      switch (index) {
        case 0:
          return "The Annunciation";
        case 1:
          return "The Visitation";
        case 2:
          return "The Nativity";
        case 3:
          return "The Presentation";
        case 4:
          return "Finding in the Temple";
        default:
          return "";
      }
    case MysterySet::Sorrowful:
      switch (index) {
        case 0:
          return "Agony in the Garden";
        case 1:
          return "Scourging at the Pillar";
        case 2:
          return "Crowning with Thorns";
        case 3:
          return "Carrying of the Cross";
        case 4:
          return "The Crucifixion";
        default:
          return "";
      }
    case MysterySet::Glorious:
      switch (index) {
        case 0:
          return "The Resurrection";
        case 1:
          return "The Ascension";
        case 2:
          return "Descent of the Holy Spirit";
        case 3:
          return "Assumption of Mary";
        case 4:
          return "Coronation of Mary";
        default:
          return "";
      }
    case MysterySet::Luminous:
      switch (index) {
        case 0:
          return "Baptism of Jesus";
        case 1:
          return "Wedding at Cana";
        case 2:
          return "Proclamation of the Kingdom";
        case 3:
          return "The Transfiguration";
        case 4:
          return "Institution of the Eucharist";
        default:
          return "";
      }
    default:
      return "";
  }
}

// Scripture references for each mystery
inline const char* getMysteryScripture(MysterySet set, int index) {
  switch (set) {
    case MysterySet::Joyful:
      switch (index) {
        case 0:
          return "Luke 1:26-38";
        case 1:
          return "Luke 1:39-56";
        case 2:
          return "Luke 2:1-21";
        case 3:
          return "Luke 2:22-38";
        case 4:
          return "Luke 2:41-52";
        default:
          return "";
      }
    case MysterySet::Sorrowful:
      switch (index) {
        case 0:
          return "Matthew 26:36-56";
        case 1:
          return "Matthew 27:26";
        case 2:
          return "Matthew 27:29";
        case 3:
          return "John 19:17";
        case 4:
          return "Luke 23:33-46";
        default:
          return "";
      }
    case MysterySet::Glorious:
      switch (index) {
        case 0:
          return "John 20:1-29";
        case 1:
          return "Acts 1:9-11";
        case 2:
          return "Acts 2:1-13";
        case 3:
          return "Rev. 12:1";
        case 4:
          return "Rev. 12:1";
        default:
          return "";
      }
    case MysterySet::Luminous:
      switch (index) {
        case 0:
          return "Matthew 3:13-17";
        case 1:
          return "John 2:1-12";
        case 2:
          return "Mark 1:14-15";
        case 3:
          return "Matthew 17:1-8";
        case 4:
          return "Matthew 26:26-28";
        default:
          return "";
      }
    default:
      return "";
  }
}

// Bead position within the rosary
// The rosary progression is modeled as a linear sequence of steps
enum class BeadType : uint8_t {
  SignOfCross,
  ApostlesCreed,
  OurFather,
  HailMary,
  GloryBe,
  FatimaPrayer,
  MysteryAnnounce,
  HailHolyQueen,
  FinalPrayer,
};

inline const char* getBeadTypeName(BeadType type) {
  switch (type) {
    case BeadType::SignOfCross:
      return "Sign of the Cross";
    case BeadType::ApostlesCreed:
      return "Apostles' Creed";
    case BeadType::OurFather:
      return "Our Father";
    case BeadType::HailMary:
      return "Hail Mary";
    case BeadType::GloryBe:
      return "Glory Be";
    case BeadType::FatimaPrayer:
      return "Fatima Prayer";
    case BeadType::MysteryAnnounce:
      return "Mystery";
    case BeadType::HailHolyQueen:
      return "Hail Holy Queen";
    case BeadType::FinalPrayer:
      return "Final Prayer";
    default:
      return "";
  }
}

// A single step in the rosary sequence
struct RosaryStep {
  BeadType type;
  int8_t decadeIndex;   // -1 for intro/outro, 0-4 for decades
  int8_t hailMaryIndex; // 0-9 for Hail Marys within a decade, -1 otherwise
};

// Total number of steps in the full rosary
// Sign of Cross + Creed + Our Father + 3 Hail Mary + Glory Be
// + 5 * (Mystery + Our Father + 10 Hail Mary + Glory Be + Fatima)
// + Hail Holy Queen + Final Prayer + Sign of Cross
// = 1 + 1 + 1 + 3 + 1 + 5*(1+1+10+1+1) + 1 + 1 + 1 = 80
static constexpr int TOTAL_STEPS = 80;

inline RosaryStep getStep(int stepIndex) {
  RosaryStep step = {BeadType::SignOfCross, -1, -1};

  if (stepIndex == 0) {
    step.type = BeadType::SignOfCross;
    return step;
  }
  if (stepIndex == 1) {
    step.type = BeadType::ApostlesCreed;
    return step;
  }
  if (stepIndex == 2) {
    step.type = BeadType::OurFather;
    step.decadeIndex = -1;
    return step;
  }
  // 3 introductory Hail Marys (steps 3, 4, 5)
  if (stepIndex >= 3 && stepIndex <= 5) {
    step.type = BeadType::HailMary;
    step.decadeIndex = -1;
    step.hailMaryIndex = stepIndex - 3;
    return step;
  }
  if (stepIndex == 6) {
    step.type = BeadType::GloryBe;
    step.decadeIndex = -1;
    return step;
  }

  // Decades: steps 7..76 (5 decades * 14 steps each)
  int decadeOffset = stepIndex - 7;
  if (decadeOffset >= 0 && decadeOffset < 70) {
    int decade = decadeOffset / 14;
    int withinDecade = decadeOffset % 14;
    step.decadeIndex = decade;

    if (withinDecade == 0) {
      step.type = BeadType::MysteryAnnounce;
    } else if (withinDecade == 1) {
      step.type = BeadType::OurFather;
    } else if (withinDecade >= 2 && withinDecade <= 11) {
      step.type = BeadType::HailMary;
      step.hailMaryIndex = withinDecade - 2;
    } else if (withinDecade == 12) {
      step.type = BeadType::GloryBe;
    } else {
      step.type = BeadType::FatimaPrayer;
    }
    return step;
  }

  // Closing prayers
  if (stepIndex == 77) {
    step.type = BeadType::HailHolyQueen;
    return step;
  }
  if (stepIndex == 78) {
    step.type = BeadType::FinalPrayer;
    return step;
  }
  if (stepIndex == 79) {
    step.type = BeadType::SignOfCross;
    return step;
  }

  return step;
}

// Prayer texts
namespace Prayers {

inline const char* signOfTheCross() {
  return "In the name of the Father, and of the Son, and of the Holy Spirit. Amen.";
}

inline const char* apostlesCreed() {
  return "I believe in God, the Father Almighty, Creator of heaven and earth; "
         "and in Jesus Christ, His only Son, our Lord; who was conceived by the Holy Spirit, "
         "born of the Virgin Mary; suffered under Pontius Pilate, was crucified, died and was buried. "
         "He descended into hell; the third day He rose again from the dead; He ascended into heaven, "
         "and is seated at the right hand of God the Father Almighty; from thence He shall come to judge "
         "the living and the dead. I believe in the Holy Spirit, the Holy Catholic Church, the communion "
         "of Saints, the forgiveness of sins, the resurrection of the body, and life everlasting. Amen.";
}

inline const char* ourFather() {
  return "Our Father, who art in heaven, hallowed be Thy name; Thy kingdom come; "
         "Thy will be done on earth as it is in heaven. Give us this day our daily bread; "
         "and forgive us our trespasses as we forgive those who trespass against us; "
         "and lead us not into temptation, but deliver us from evil. Amen.";
}

inline const char* hailMary() {
  return "Hail Mary, full of grace, the Lord is with thee. Blessed art thou amongst women, "
         "and blessed is the fruit of thy womb, Jesus. Holy Mary, Mother of God, pray for us sinners, "
         "now and at the hour of our death. Amen.";
}

inline const char* gloryBe() {
  return "Glory be to the Father, and to the Son, and to the Holy Spirit. "
         "As it was in the beginning, is now, and ever shall be, world without end. Amen.";
}

inline const char* fatimaPrayer() {
  return "O my Jesus, forgive us our sins, save us from the fires of hell, "
         "lead all souls to heaven, especially those in most need of Thy mercy. Amen.";
}

inline const char* hailHolyQueen() {
  return "Hail, Holy Queen, Mother of mercy, our life, our sweetness and our hope. "
         "To thee do we cry, poor banished children of Eve. To thee do we send up our sighs, "
         "mourning and weeping in this valley of tears. Turn, then, most gracious Advocate, "
         "thine eyes of mercy toward us, and after this, our exile, show unto us the blessed "
         "fruit of thy womb, Jesus. O clement, O loving, O sweet Virgin Mary! "
         "Pray for us, O holy Mother of God, that we may be made worthy of the promises of Christ. Amen.";
}

inline const char* finalPrayer() {
  return "Let us pray. O God, whose Only Begotten Son, by His life, death, and resurrection, "
         "has purchased for us the rewards of eternal life; grant, we beseech Thee, that by meditating "
         "upon these mysteries of the Most Holy Rosary of the Blessed Virgin Mary, we may imitate what "
         "they contain and obtain what they promise, through the same Christ our Lord. Amen.";
}

inline const char* getPrayerText(BeadType type) {
  switch (type) {
    case BeadType::SignOfCross:
      return signOfTheCross();
    case BeadType::ApostlesCreed:
      return apostlesCreed();
    case BeadType::OurFather:
      return ourFather();
    case BeadType::HailMary:
      return hailMary();
    case BeadType::GloryBe:
      return gloryBe();
    case BeadType::FatimaPrayer:
      return fatimaPrayer();
    case BeadType::HailHolyQueen:
      return hailHolyQueen();
    case BeadType::FinalPrayer:
      return finalPrayer();
    case BeadType::MysteryAnnounce:
      return "";  // Mystery text is dynamic
    default:
      return "";
  }
}

}  // namespace Prayers

// Prayer reference list for quick access
static constexpr int PRAYER_REFERENCE_COUNT = 7;

inline const char* getPrayerReferenceName(int index) {
  switch (index) {
    case 0:
      return "Sign of the Cross";
    case 1:
      return "Apostles' Creed";
    case 2:
      return "Our Father";
    case 3:
      return "Hail Mary";
    case 4:
      return "Glory Be";
    case 5:
      return "Fatima Prayer";
    case 6:
      return "Hail Holy Queen";
    default:
      return "";
  }
}

inline const char* getPrayerReferenceText(int index) {
  switch (index) {
    case 0:
      return Prayers::signOfTheCross();
    case 1:
      return Prayers::apostlesCreed();
    case 2:
      return Prayers::ourFather();
    case 3:
      return Prayers::hailMary();
    case 4:
      return Prayers::gloryBe();
    case 5:
      return Prayers::fatimaPrayer();
    case 6:
      return Prayers::hailHolyQueen();
    default:
      return "";
  }
}

}  // namespace RosaryData
