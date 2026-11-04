#pragma once
namespace surround_ui {
// A job is completed synchronously before its source scratch buffer can be reused.
// Separate SDK-independent lifetime logic so unsupported/partial paths are tested.
template<class Begin,class Add,class End,class Cancel>
bool submit_display_job(Begin begin,Add add,End end,Cancel cancel){
    auto job=begin();if(!job)return false;
    for(unsigned tile=0;tile<4;++tile)if(!add(job,tile)){cancel(job);return false;}
    return end(job); // end consumes the job; never cancel an already-ended handle.
}
}
